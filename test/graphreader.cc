#include "baldr/graphreader.h"
#include "baldr/connectivity_map.h"
#include "baldr/tilehierarchy.h"
#include "midgard/sequence.h"

#include <boost/property_tree/ptree.hpp>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace valhalla::baldr;

namespace {

class test_cache : public SimpleTileCache {
public:
  using SimpleTileCache::cache_size_;
  using SimpleTileCache::max_cache_size_;
  using SimpleTileCache::SimpleTileCache;
};

TEST(SimpleCache, QueryByPointOutOfRangeLL) {
  boost::property_tree::ptree pt;
  pt.put("tile_dir", "test/gphrdr_test");
  GraphReader reader(pt);

  // Latitude out of range
  EXPECT_TRUE(reader.GetGraphTile({60.0f, 100.0f}) == nullptr)
      << "Out of range LL should return nullptr";

  // Out of range longitude
  EXPECT_TRUE(reader.GetGraphTile({460.0f, 60.0f}) == nullptr)
      << "Out of range LL should return nullptr";
}

test_cache make_cache(size_t cache_size) {
  return {cache_size};
}

TEST(SimpleCache, CacheLimitsZeroSizeOvercommited) {
  EXPECT_FALSE(make_cache(0).OverCommitted());
}

TEST(SimpleCache, CacheLimitsMinSizeOvercommited) {
  EXPECT_FALSE(make_cache(1).OverCommitted());
}

TEST(SimpleCache, CacheLimitsOvercommitBasic) {
  auto cache = make_cache(10);
  cache.cache_size_ = 20;
  EXPECT_TRUE(cache.OverCommitted());

  cache.cache_size_ = 1;
  cache.max_cache_size_ = 0;
  EXPECT_TRUE(cache.OverCommitted());
}

TEST(SimpleCache, CacheLimitsNoOvercommitAfterClear) {
  auto cache = make_cache(10);
  cache.cache_size_ = cache.max_cache_size_ + 1;
  EXPECT_TRUE(cache.OverCommitted());
  cache.Clear();
  EXPECT_FALSE(cache.OverCommitted());
}

void touch_tile(const uint32_t tile_id, const std::string& tile_dir, uint8_t level) {
  auto suffix = GraphTile::FileSuffix({tile_id, level, 0});
  std::filesystem::path fullpath{tile_dir};
  fullpath.append(suffix);
  std::filesystem::create_directories(fullpath.parent_path());
  std::ofstream{fullpath, std::ios::binary | std::ios::app};
}

TEST(ConnectivityMap, Basic) {
  // get the property tree to create some tiles
  boost::property_tree::ptree pt;
  pt.put("tile_dir", "test/gphrdr_test");
  std::string tile_dir = pt.get<std::string>("tile_dir");
  for (const auto& level : {TileHierarchy::levels()[2], TileHierarchy::GetTransitLevel()}) {
    std::filesystem::remove_all(tile_dir);

    // looks like this (XX) means no tile there:
    /*
     *     XX d1 XX XX
     *     XX d0 XX XX
     *     a2 XX c0 XX
     *     a0 a1 XX b0
     *
     */

    // create some empty files with tile names
    uint32_t a0 = 0;
    touch_tile(a0, tile_dir, level.level);

    uint32_t a1 = level.tiles.RightNeighbor(a0);
    touch_tile(a1, tile_dir, level.level);

    uint32_t a2 = level.tiles.TopNeighbor(a0);
    touch_tile(a2, tile_dir, level.level);

    uint32_t b0 = level.tiles.RightNeighbor(level.tiles.RightNeighbor(a1));
    touch_tile(b0, tile_dir, level.level);

    uint32_t c0 = level.tiles.TopNeighbor(level.tiles.RightNeighbor(a1));
    touch_tile(c0, tile_dir, level.level);

    uint32_t d0 = level.tiles.TopNeighbor(level.tiles.RightNeighbor(a2));
    touch_tile(d0, tile_dir, level.level);

    uint32_t d1 = level.tiles.TopNeighbor(d0);
    touch_tile(d1, tile_dir, level.level);

    // check that it looks right
    connectivity_map_t conn(pt);

    EXPECT_EQ(conn.get_color({a0, level.level, 0}), conn.get_color({a1, level.level, 0}))
        << "a's should be connected";
    EXPECT_EQ(conn.get_color({a0, level.level, 0}), conn.get_color({a2, level.level, 0}))
        << "a's should be connected";
    EXPECT_EQ(conn.get_color({a1, level.level, 0}), conn.get_color({a2, level.level, 0}))
        << "a's should be connected";
    EXPECT_EQ(conn.get_color({d0, level.level, 0}), conn.get_color({d1, level.level, 0}))
        << "d's should be connected";
    EXPECT_NE(conn.get_color({c0, level.level, 0}), conn.get_color({a1, level.level, 0}))
        << "c is disjoint";
    EXPECT_NE(conn.get_color({b0, level.level, 0}), conn.get_color({a0, level.level, 0}))
        << "b is disjoint";
    EXPECT_NE(conn.get_color({a2, level.level, 0}), conn.get_color({d0, level.level, 0}))
        << "a is disjoint from d";

    std::filesystem::remove_all(tile_dir);
  }
}

class TestGraphMemory final : public GraphMemory {
public:
  TestGraphMemory() : memory_(sizeof(GraphTileHeader)) {
    data = const_cast<char*>(memory_.data());
    size = memory_.size();
  }

private:
  const std::vector<char> memory_;
};

struct TestGraphTile : public GraphTile {
  TestGraphTile(GraphId id, size_t size) {
    memory_ = std::make_unique<const TestGraphMemory>();
    header_ = reinterpret_cast<GraphTileHeader*>(memory_->data);
    header_->set_graphid(id);
    header_->set_end_offset(size);
  }
};

static void
CheckGraphTile(const graph_tile_ptr& tile, const GraphId& expected_id, size_t expected_size) {
  ASSERT_NE(tile, nullptr);
  EXPECT_EQ(tile->header()->graphid().value, expected_id.value);
  EXPECT_EQ(tile->header()->end_offset(), expected_size);
}

TEST(SimpleCache, Clear) {
  SimpleTileCache cache(400);

  GraphId id1(100, 2, 0);
  auto tile1 = cache.Put(id1, graph_tile_ptr{new TestGraphTile(id1, 123)}, 123);
  EXPECT_EQ(cache.Get(id1), tile1);
  CheckGraphTile(tile1, id1, 123);

  EXPECT_FALSE(cache.OverCommitted());

  GraphId id2(300, 1, 0);
  graph_tile_ptr tile2 = cache.Put(id2, graph_tile_ptr{new TestGraphTile(id2, 200)}, 200);
  EXPECT_EQ(cache.Get(id2), tile2);
  CheckGraphTile(tile2, id2, 200);

  EXPECT_FALSE(cache.OverCommitted());

  GraphId id3(1000, 0, 0);
  auto tile3 = cache.Put(id3, graph_tile_ptr{new TestGraphTile(id3, 500)}, 500);
  EXPECT_EQ(cache.Get(id3), tile3);
  CheckGraphTile(tile3, id3, 500);

  EXPECT_TRUE(cache.OverCommitted());

  // Check if inserted values are correct

  auto returned2 = cache.Get({300, 1, 0});
  CheckGraphTile(returned2, id2, 200);
  auto returned1 = cache.Get({100, 2, 0});
  CheckGraphTile(returned1, id1, 123);
  auto returned3 = cache.Get({1000, 0, 0});
  CheckGraphTile(returned3, id3, 500);

  EXPECT_TRUE(cache.Contains(id1));
  EXPECT_TRUE(cache.Contains(id2));
  EXPECT_TRUE(cache.Contains(id3));

  cache.Clear();

  EXPECT_FALSE(cache.OverCommitted());

  EXPECT_FALSE(cache.Contains(id1));
  EXPECT_FALSE(cache.Contains(id2));
  EXPECT_FALSE(cache.Contains(id3));

  EXPECT_EQ(cache.Get(id1), nullptr);
  EXPECT_EQ(cache.Get(id2), nullptr);
  EXPECT_EQ(cache.Get(id3), nullptr);
}

TEST(SimpleCache, Trim) {
  SimpleTileCache cache(400);

  GraphId id1(100, 2, 0);
  auto tile1 = cache.Put(id1, graph_tile_ptr{new TestGraphTile(id1, 123)}, 123);
  EXPECT_EQ(cache.Get(id1), tile1);
  CheckGraphTile(tile1, id1, 123);

  EXPECT_FALSE(cache.OverCommitted());

  GraphId id2(300, 1, 0);
  auto tile2 = cache.Put(id2, graph_tile_ptr{new TestGraphTile(id2, 200)}, 200);
  EXPECT_EQ(cache.Get(id2), tile2);
  CheckGraphTile(tile2, id2, 200);

  EXPECT_FALSE(cache.OverCommitted());

  GraphId id3(1000, 0, 0);
  auto tile3 = cache.Put(id3, graph_tile_ptr{new TestGraphTile(id3, 500)}, 500);
  EXPECT_EQ(cache.Get(id3), tile3);
  CheckGraphTile(tile3, id3, 500);

  EXPECT_TRUE(cache.OverCommitted());

  // Check if inserted values are correct

  auto returned2 = cache.Get({300, 1, 0});
  CheckGraphTile(returned2, id2, 200);
  auto returned1 = cache.Get({100, 2, 0});
  CheckGraphTile(returned1, id1, 123);
  auto returned3 = cache.Get({1000, 0, 0});
  CheckGraphTile(returned3, id3, 500);

  EXPECT_TRUE(cache.Contains(id1));
  EXPECT_TRUE(cache.Contains(id2));
  EXPECT_TRUE(cache.Contains(id3));

  cache.Trim();

  EXPECT_FALSE(cache.OverCommitted());

  EXPECT_FALSE(cache.Contains(id1));
  EXPECT_FALSE(cache.Contains(id2));
  EXPECT_FALSE(cache.Contains(id3));

  EXPECT_EQ(cache.Get(id1), nullptr);
  EXPECT_EQ(cache.Get(id2), nullptr);
  EXPECT_EQ(cache.Get(id3), nullptr);
}

TEST(CacheLruHard, Creation) {
  TileCacheLRU zero_limit_cache(0, TileCacheLRU::MemoryLimitControl::HARD);
  TileCacheLRU cache(1023, TileCacheLRU::MemoryLimitControl::HARD);
  TileCacheLRU big_cache(1073741824, TileCacheLRU::MemoryLimitControl::HARD);
}

TEST(CacheLruHard, InsertSingleItemBiggerThanCacheSize) {
  TileCacheLRU cache(1023, TileCacheLRU::MemoryLimitControl::HARD);

  GraphId id1(100, 2, 0);

  EXPECT_THROW(cache.Put(id1, graph_tile_ptr{new TestGraphTile(id1, 2000)}, 2000),
               std::runtime_error);
  EXPECT_EQ(cache.Get(id1), nullptr);
  EXPECT_FALSE(cache.Contains(id1));
}

TEST(CacheLruHard, InsertCacheFullOneshot) {
  const size_t tile1_size = 1234;

  TileCacheLRU cache(tile1_size, TileCacheLRU::MemoryLimitControl::HARD);

  GraphId tile1_id(1000, 1, 0);
  auto tile1 =
      cache.Put(tile1_id, graph_tile_ptr{new TestGraphTile(tile1_id, tile1_size)}, tile1_size);
  EXPECT_EQ(cache.Get(tile1_id), tile1);
  EXPECT_FALSE(cache.OverCommitted());

  CheckGraphTile(tile1, tile1_id, tile1_size);
  EXPECT_TRUE(cache.Contains(tile1_id));
}

TEST(CacheLruHard, InsertCacheFull) {
  TileCacheLRU cache(10000, TileCacheLRU::MemoryLimitControl::HARD);

  const size_t tile1_size = 4000;
  GraphId tile1_id(1000, 1, 0);
  auto tile1 =
      cache.Put(tile1_id, graph_tile_ptr{new TestGraphTile(tile1_id, tile1_size)}, tile1_size);
  EXPECT_EQ(cache.Get(tile1_id), tile1);
  CheckGraphTile(tile1, tile1_id, tile1_size);

  const size_t tile2_size = 6000;
  GraphId tile2_id(33, 2, 0);
  auto tile2 =
      cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile2_size)}, tile2_size);
  EXPECT_EQ(cache.Get(tile2_id), tile2);
  CheckGraphTile(tile2, tile2_id, tile2_size);

  EXPECT_FALSE(cache.OverCommitted());

  EXPECT_TRUE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));
}

TEST(CacheLruHard, InsertNoEviction) {
  TileCacheLRU cache(1023, TileCacheLRU::MemoryLimitControl::HARD);

  GraphId id1(100, 2, 0);
  auto tile1 = cache.Put(id1, graph_tile_ptr{new TestGraphTile(id1, 123)}, 123);
  EXPECT_EQ(cache.Get(id1), tile1);
  CheckGraphTile(tile1, id1, 123);

  GraphId id2(300, 1, 0);
  auto tile2 = cache.Put(id2, graph_tile_ptr{new TestGraphTile(id2, 200)}, 200);
  EXPECT_EQ(cache.Get(id2), tile2);
  CheckGraphTile(tile2, id2, 200);

  GraphId id3(1000, 0, 0);
  auto tile3 = cache.Put(id3, graph_tile_ptr{new TestGraphTile(id3, 500)}, 500);
  EXPECT_EQ(cache.Get(id3), tile3);
  CheckGraphTile(tile3, id3, 500);

  // Check if inserted values are correct

  auto returned2 = cache.Get({300, 1, 0});
  CheckGraphTile(returned2, id2, 200);

  auto returned1 = cache.Get({100, 2, 0});
  CheckGraphTile(returned1, id1, 123);

  auto returned3 = cache.Get({1000, 0, 0});
  CheckGraphTile(returned3, id3, 500);

  // Make sure tiles we have never cached are not found
  EXPECT_EQ(cache.Get({1345, 1, 0}), nullptr);
  EXPECT_EQ(cache.Get({100, 1, 0}), nullptr);
  EXPECT_EQ(cache.Get({0, 0, 0}), nullptr);
}

TEST(CacheLruHard, InsertWithEvictionBasic) {
  TileCacheLRU cache(500, TileCacheLRU::MemoryLimitControl::HARD);

  GraphId tile1_id(1000, 1, 0);
  const size_t tile1_size = 200;
  cache.Put(tile1_id, graph_tile_ptr{new TestGraphTile(tile1_id, tile1_size)}, tile1_size);

  GraphId tile2_id(300, 2, 0);
  const size_t tile2_size = 250;
  cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile2_size)}, tile2_size);

  GraphId tile3_id(1, 1, 0);
  const size_t tile3_size = 45;
  cache.Put(tile3_id, graph_tile_ptr{new TestGraphTile(tile3_id, tile3_size)}, tile3_size);

  EXPECT_TRUE(cache.Contains(tile3_id));
  EXPECT_TRUE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));

  // Add an insertion which now requires the eviction
  // The first inserted items should be evicted here (id1)

  GraphId tile4_id(400, 2, 0);
  const size_t tile4_size = 20;
  cache.Put(tile4_id, graph_tile_ptr{new TestGraphTile(tile4_id, tile4_size)}, tile4_size);

  EXPECT_FALSE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));
  EXPECT_TRUE(cache.Contains(tile3_id));
  EXPECT_TRUE(cache.Contains(tile4_id));

  // Now we access an entry that would be evicted next
  // to promote its position in the LRU list and change eviction order

  auto returned2 = cache.Get(tile2_id);
  CheckGraphTile(returned2, tile2_id, tile2_size);

  GraphId tile5_id(999, 1, 0);
  const size_t tile5_size = 200;
  cache.Put(tile5_id, graph_tile_ptr{new TestGraphTile(tile5_id, tile5_size)}, tile5_size);

  // Expecting the next eviction of tile3 since tile1 has been just accessed

  EXPECT_FALSE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));
  EXPECT_FALSE(cache.Contains(tile3_id));
  EXPECT_TRUE(cache.Contains(tile4_id));
  EXPECT_TRUE(cache.Contains(tile5_id));

  EXPECT_EQ(cache.Get(tile1_id), nullptr);
  EXPECT_EQ(cache.Get(tile3_id), nullptr);

  CheckGraphTile(cache.Get(tile2_id), tile2_id, tile2_size);
  CheckGraphTile(cache.Get(tile4_id), tile4_id, tile4_size);
  CheckGraphTile(cache.Get(tile5_id), tile5_id, tile5_size);
}

TEST(CacheLruHard, OverwriteSameSize) {
  TileCacheLRU cache(500, TileCacheLRU::MemoryLimitControl::HARD);

  GraphId tile1_id(1000, 1, 0);
  const size_t tile1_size = 200;
  cache.Put(tile1_id, graph_tile_ptr{new TestGraphTile(tile1_id, tile1_size)}, tile1_size);

  GraphId tile2_id(300, 2, 0);
  const size_t tile2_size = 250;
  cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile2_size)}, tile2_size);

  GraphId tile3_id(1, 1, 0);
  const size_t tile3_size = 45;
  cache.Put(tile3_id, graph_tile_ptr{new TestGraphTile(tile3_id, tile3_size)}, tile3_size);

  // overwrite with a new object and same size
  cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile2_size)}, tile2_size);

  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);
  CheckGraphTile(cache.Get(tile2_id), tile2_id, tile2_size);
  CheckGraphTile(cache.Get(tile3_id), tile3_id, tile3_size);
  EXPECT_TRUE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));
  EXPECT_TRUE(cache.Contains(tile3_id));
}

TEST(CacheLruHard, OverwriteSmallerSize) {
  TileCacheLRU cache(500, TileCacheLRU::MemoryLimitControl::HARD);

  GraphId tile1_id(1000, 1, 0);
  const size_t tile1_size = 200;
  cache.Put(tile1_id, graph_tile_ptr{new TestGraphTile(tile1_id, tile1_size)}, tile1_size);

  GraphId tile2_id(300, 2, 0);
  const size_t tile2_size = 250;
  cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile2_size)}, tile2_size);

  GraphId tile3_id(1, 1, 0);
  const size_t tile3_size = 45;
  cache.Put(tile3_id, graph_tile_ptr{new TestGraphTile(tile3_id, tile3_size)}, tile3_size);

  const size_t tile4_size = 8;
  cache.Put(tile3_id, graph_tile_ptr{new TestGraphTile(tile3_id, tile4_size)}, tile4_size);

  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);
  CheckGraphTile(cache.Get(tile2_id), tile2_id, tile2_size);
  CheckGraphTile(cache.Get(tile3_id), tile3_id, tile4_size);
  EXPECT_TRUE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));
  EXPECT_TRUE(cache.Contains(tile3_id));
}

TEST(CacheLruHard, OverwriteBiggerSizeNoEviction) {
  TileCacheLRU cache(500, TileCacheLRU::MemoryLimitControl::HARD);

  GraphId tile1_id(1000, 1, 0);
  const size_t tile1_size = 200;
  cache.Put(tile1_id, graph_tile_ptr{new TestGraphTile(tile1_id, tile1_size)}, tile1_size);

  GraphId tile2_id(300, 2, 0);
  const size_t tile2_size = 250;
  cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile2_size)}, tile2_size);

  GraphId tile3_id(1, 1, 0);
  const size_t tile3_size = 20;
  cache.Put(tile3_id, graph_tile_ptr{new TestGraphTile(tile3_id, tile3_size)}, tile3_size);

  const size_t tile4_size = 45;
  cache.Put(tile3_id, graph_tile_ptr{new TestGraphTile(tile3_id, tile4_size)}, tile4_size);

  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);
  CheckGraphTile(cache.Get(tile2_id), tile2_id, tile2_size);
  CheckGraphTile(cache.Get(tile3_id), tile3_id, tile4_size);
  EXPECT_TRUE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));
  EXPECT_TRUE(cache.Contains(tile3_id));
}

TEST(CacheLruHard, OverwriteBiggerSizeEvictionOne) {
  TileCacheLRU cache(500, TileCacheLRU::MemoryLimitControl::HARD);

  GraphId tile1_id(1000, 1, 0);
  const size_t tile1_size = 200;
  cache.Put(tile1_id, graph_tile_ptr{new TestGraphTile(tile1_id, tile1_size)}, tile1_size);

  GraphId tile2_id(300, 2, 0);
  const size_t tile2_size = 250;
  cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile2_size)}, tile2_size);

  GraphId tile3_id(1, 1, 0);
  const size_t tile3_size = 45;
  cache.Put(tile3_id, graph_tile_ptr{new TestGraphTile(tile3_id, tile3_size)}, tile3_size);

  EXPECT_TRUE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));
  EXPECT_TRUE(cache.Contains(tile3_id));

  // Add an insertion which now requires the eviction
  // The first inserted items should be evicted here (id1)
  // Note, we are not inserting a new entry but updating the existing one (id2)

  const size_t tile4_size = 260;
  cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile4_size)}, tile4_size);

  EXPECT_FALSE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));
  EXPECT_TRUE(cache.Contains(tile3_id));

  EXPECT_EQ(cache.Get(tile1_id), nullptr);
  CheckGraphTile(cache.Get(tile2_id), tile2_id, tile4_size);
  CheckGraphTile(cache.Get(tile3_id), tile3_id, tile3_size);
}

TEST(CacheLruHard, OverwriteBiggerSizeEvictionMultiple) {
  TileCacheLRU cache(500, TileCacheLRU::MemoryLimitControl::HARD);

  GraphId tile1_id(1000, 1, 0);
  const size_t tile1_size = 200;
  cache.Put(tile1_id, graph_tile_ptr{new TestGraphTile(tile1_id, tile1_size)}, tile1_size);

  GraphId tile2_id(300, 2, 0);
  const size_t tile2_size = 250;
  cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile2_size)}, tile2_size);

  GraphId tile3_id(1, 1, 0);
  const size_t tile3_size = 45;
  cache.Put(tile3_id, graph_tile_ptr{new TestGraphTile(tile3_id, tile3_size)}, tile3_size);

  EXPECT_TRUE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));
  EXPECT_TRUE(cache.Contains(tile3_id));

  // Add an insertion which now requires the eviction
  // The first inserted items should be evicted here (id1)
  // Note, we are not inserting a new entry but updating the existing one (id2)
  // The new size is picked such that it should be the only item remaining in the cache

  const size_t tile4_size = 480;
  cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile4_size)}, tile4_size);

  EXPECT_FALSE(cache.Contains(tile1_id));
  EXPECT_FALSE(cache.Contains(tile3_id));
  EXPECT_TRUE(cache.Contains(tile2_id));

  EXPECT_EQ(cache.Get(tile1_id), nullptr);
  EXPECT_EQ(cache.Get(tile3_id), nullptr);
  CheckGraphTile(cache.Get(tile2_id), tile2_id, tile4_size);
}

TEST(CacheLruHard, InsertWithEvictionEntireCache) {
  TileCacheLRU cache(1000, TileCacheLRU::MemoryLimitControl::HARD);

  GraphId tile1_id(1000, 1, 0);
  const size_t tile1_size = 200;
  cache.Put(tile1_id, graph_tile_ptr{new TestGraphTile(tile1_id, tile1_size)}, tile1_size);

  GraphId tile2_id(300, 2, 0);
  const size_t tile2_size = 300;
  cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile2_size)}, tile2_size);

  GraphId tile3_id(1, 1, 0);
  const size_t tile3_size = 900;
  cache.Put(tile3_id, graph_tile_ptr{new TestGraphTile(tile3_id, tile3_size)}, tile3_size);

  EXPECT_TRUE(cache.Contains(tile3_id));
  EXPECT_FALSE(cache.Contains(tile1_id));
  EXPECT_FALSE(cache.Contains(tile2_id));

  auto returned3 = cache.Get(tile3_id);
  CheckGraphTile(returned3, tile3_id, tile3_size);

  EXPECT_EQ(cache.Get(tile1_id), nullptr);
  EXPECT_EQ(cache.Get(tile2_id), nullptr);
}

TEST(CacheLruHard, MixedInsertOverwrite) {
  TileCacheLRU cache(4000, TileCacheLRU::MemoryLimitControl::HARD);

  GraphId tile1_id(1000, 1, 0);
  const size_t tile1_size = 1000;
  cache.Put(tile1_id, graph_tile_ptr{new TestGraphTile(tile1_id, tile1_size)}, tile1_size);

  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);

  GraphId tile2_id(300, 2, 0);
  const size_t tile2_size = 300;
  cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile2_size)}, tile2_size);

  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);

  GraphId tile3_id(1, 1, 0);
  const size_t tile3_size = 900;
  cache.Put(tile3_id, graph_tile_ptr{new TestGraphTile(tile3_id, tile3_size)}, tile3_size);

  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);
  CheckGraphTile(cache.Get(tile3_id), tile3_id, tile3_size);

  const size_t tile4_size = 123;
  cache.Put(tile3_id, graph_tile_ptr{new TestGraphTile(tile3_id, tile4_size)}, tile4_size);

  GraphId tile5_id(1234, 1, 0);
  const size_t tile5_size = 200;
  cache.Put(tile5_id, graph_tile_ptr{new TestGraphTile(tile5_id, tile5_size)}, tile5_size);

  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);
  CheckGraphTile(cache.Get(tile2_id), tile2_id, tile2_size);
  CheckGraphTile(cache.Get(tile3_id), tile3_id, tile4_size);
  CheckGraphTile(cache.Get(tile5_id), tile5_id, tile5_size);
}

TEST(CacheLruHard, MixedInsertOverwriteEvict) {
  TileCacheLRU cache(500, TileCacheLRU::MemoryLimitControl::HARD);

  GraphId tile1_id(1000, 1, 0);
  const size_t tile1_size = 200;
  cache.Put(tile1_id, graph_tile_ptr{new TestGraphTile(tile1_id, tile1_size)}, tile1_size);

  GraphId tile2_id(300, 2, 0);
  const size_t tile2_size = 250;
  cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile2_size)}, tile2_size);

  // bump tile1 (with get)
  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);

  GraphId tile3_id(1, 1, 0);
  const size_t tile3_size = 45;
  cache.Put(tile3_id, graph_tile_ptr{new TestGraphTile(tile3_id, tile3_size)}, tile3_size);

  EXPECT_TRUE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));
  EXPECT_TRUE(cache.Contains(tile3_id));

  // Add an insertion which now requires the eviction
  // Expecting id2 to get evicted
  // Note, we are not inserting a new entry but updating the existing one (id3)

  const size_t tile4_size = 255;
  cache.Put(tile3_id, graph_tile_ptr{new TestGraphTile(tile3_id, tile4_size)}, tile4_size);

  EXPECT_TRUE(cache.Contains(tile1_id));
  EXPECT_FALSE(cache.Contains(tile2_id));
  EXPECT_TRUE(cache.Contains(tile3_id));

  EXPECT_EQ(cache.Get(tile2_id), nullptr);
  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);
  CheckGraphTile(cache.Get(tile3_id), tile3_id, tile4_size);
}

TEST(CacheLruHard, ClearBasic) {
  TileCacheLRU cache(2000, TileCacheLRU::MemoryLimitControl::HARD);

  GraphId tile1_id(10, 1, 0);
  const size_t tile1_size = 500;
  cache.Put(tile1_id, graph_tile_ptr{new TestGraphTile(tile1_id, tile1_size)}, tile1_size);

  GraphId tile2_id(300, 2, 0);
  const size_t tile2_size = 123;
  cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile2_size)}, tile2_size);

  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);
  CheckGraphTile(cache.Get(tile2_id), tile2_id, tile2_size);
  EXPECT_TRUE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));

  cache.Clear();

  EXPECT_FALSE(cache.Contains(tile1_id));
  EXPECT_FALSE(cache.Contains(tile2_id));
  EXPECT_EQ(cache.Get(tile1_id), nullptr);
  EXPECT_EQ(cache.Get(tile2_id), nullptr);
}

TEST(CacheLruHard, TrimBasic) {
  // Trim should not have any effect on the cache with hard memory limit policy
  TileCacheLRU cache(2000, TileCacheLRU::MemoryLimitControl::HARD);

  GraphId tile1_id(10, 1, 0);
  const size_t tile1_size = 500;
  cache.Put(tile1_id, graph_tile_ptr{new TestGraphTile(tile1_id, tile1_size)}, tile1_size);

  GraphId tile2_id(300, 2, 0);
  const size_t tile2_size = 123;
  cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile2_size)}, tile2_size);

  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);
  CheckGraphTile(cache.Get(tile2_id), tile2_id, tile2_size);
  EXPECT_TRUE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));

  cache.Trim();

  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);
  CheckGraphTile(cache.Get(tile2_id), tile2_id, tile2_size);
  EXPECT_TRUE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));
}

TEST(CacheLruSoft, InsertBecomeOvercommittedTrim) {
  TileCacheLRU cache(2000, TileCacheLRU::MemoryLimitControl::SOFT);

  GraphId tile1_id(10, 1, 0);
  const size_t tile1_size = 1500;
  cache.Put(tile1_id, graph_tile_ptr{new TestGraphTile(tile1_id, tile1_size)}, tile1_size);

  EXPECT_FALSE(cache.OverCommitted());

  GraphId tile2_id(300, 2, 0);
  const size_t tile2_size = 2000;
  cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile2_size)}, tile2_size);

  EXPECT_TRUE(cache.OverCommitted());

  GraphId tile3_id(500, 1, 0);
  const size_t tile3_size = 100;
  cache.Put(tile3_id, graph_tile_ptr{new TestGraphTile(tile3_id, tile3_size)}, tile3_size);

  EXPECT_TRUE(cache.OverCommitted());

  // With soft memory limit strategy there should not be any evictions here
  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);
  CheckGraphTile(cache.Get(tile2_id), tile2_id, tile2_size);
  CheckGraphTile(cache.Get(tile3_id), tile3_id, tile3_size);
  EXPECT_TRUE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));
  EXPECT_TRUE(cache.Contains(tile3_id));

  cache.Trim();

  EXPECT_EQ(cache.Get(tile1_id), nullptr);
  EXPECT_EQ(cache.Get(tile2_id), nullptr);
  CheckGraphTile(cache.Get(tile3_id), tile3_id, tile3_size);
  EXPECT_FALSE(cache.Contains(tile1_id));
  EXPECT_FALSE(cache.Contains(tile2_id));
  EXPECT_TRUE(cache.Contains(tile3_id));
}

TEST(CacheLruSoft, InsertBecomeOvercommittedClear) {
  TileCacheLRU cache(2000, TileCacheLRU::MemoryLimitControl::SOFT);

  GraphId tile1_id(10, 1, 0);
  const size_t tile1_size = 1500;
  cache.Put(tile1_id, graph_tile_ptr{new TestGraphTile(tile1_id, tile1_size)}, tile1_size);

  EXPECT_FALSE(cache.OverCommitted());

  GraphId tile2_id(300, 2, 0);
  const size_t tile2_size = 2000;
  cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile2_size)}, tile2_size);

  EXPECT_TRUE(cache.OverCommitted());

  GraphId tile3_id(500, 1, 0);
  const size_t tile3_size = 100;
  cache.Put(tile3_id, graph_tile_ptr{new TestGraphTile(tile3_id, tile3_size)}, tile3_size);

  EXPECT_TRUE(cache.OverCommitted());

  // With soft memory limit strategy there should not be any evictions here
  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);
  CheckGraphTile(cache.Get(tile2_id), tile2_id, tile2_size);
  CheckGraphTile(cache.Get(tile3_id), tile3_id, tile3_size);
  EXPECT_TRUE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));
  EXPECT_TRUE(cache.Contains(tile3_id));

  cache.Clear();

  // Expecting clear to remove everything
  EXPECT_FALSE(cache.OverCommitted());

  EXPECT_EQ(cache.Get(tile1_id), nullptr);
  EXPECT_EQ(cache.Get(tile2_id), nullptr);
  EXPECT_EQ(cache.Get(tile3_id), nullptr);
  EXPECT_FALSE(cache.Contains(tile1_id));
  EXPECT_FALSE(cache.Contains(tile2_id));
  EXPECT_FALSE(cache.Contains(tile3_id));
}

TEST(CacheLruSoft, UndercommittedTrim) {
  TileCacheLRU cache(5000, TileCacheLRU::MemoryLimitControl::SOFT);

  GraphId tile1_id(10, 1, 0);
  const size_t tile1_size = 300;
  cache.Put(tile1_id, graph_tile_ptr{new TestGraphTile(tile1_id, tile1_size)}, tile1_size);

  EXPECT_FALSE(cache.OverCommitted());

  GraphId tile2_id(300, 2, 0);
  const size_t tile2_size = 2000;
  cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile2_size)}, tile2_size);

  EXPECT_FALSE(cache.OverCommitted());

  // With soft memory limit strategy there should not be any evictions here
  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);
  CheckGraphTile(cache.Get(tile2_id), tile2_id, tile2_size);
  EXPECT_TRUE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));

  cache.Trim();

  EXPECT_FALSE(cache.OverCommitted());

  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);
  CheckGraphTile(cache.Get(tile2_id), tile2_id, tile2_size);
  EXPECT_TRUE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));
}

TEST(CacheLruSoft, InsertWithEvictionBasic) {
  TileCacheLRU cache(500, TileCacheLRU::MemoryLimitControl::SOFT);

  GraphId tile1_id(1000, 1, 0);
  const size_t tile1_size = 200;
  cache.Put(tile1_id, graph_tile_ptr{new TestGraphTile(tile1_id, tile1_size)}, tile1_size);

  GraphId tile2_id(300, 2, 0);
  const size_t tile2_size = 250;
  cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile2_size)}, tile2_size);

  GraphId tile3_id(1, 1, 0);
  const size_t tile3_size = 50;
  cache.Put(tile3_id, graph_tile_ptr{new TestGraphTile(tile3_id, tile3_size)}, tile3_size);

  GraphId tile4_id(400, 2, 0);
  const size_t tile4_size = 270;
  cache.Put(tile4_id, graph_tile_ptr{new TestGraphTile(tile4_id, tile4_size)}, tile4_size);

  EXPECT_TRUE(cache.OverCommitted());

  // Now we access an entry that would be evicted next
  // to promote its position in the LRU list and change eviction order
  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);

  // should evict tiles 2 and 3
  cache.Trim();

  EXPECT_FALSE(cache.OverCommitted());

  EXPECT_TRUE(cache.Contains(tile1_id));
  EXPECT_FALSE(cache.Contains(tile2_id));
  EXPECT_FALSE(cache.Contains(tile3_id));
  EXPECT_TRUE(cache.Contains(tile4_id));

  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);
  CheckGraphTile(cache.Get(tile4_id), tile4_id, tile4_size);
  EXPECT_EQ(cache.Get(tile2_id), nullptr);
  EXPECT_EQ(cache.Get(tile3_id), nullptr);
}

TEST(CacheLruSoft, TrimOnExactlyFullCache) {
  TileCacheLRU cache(100000, TileCacheLRU::MemoryLimitControl::SOFT);

  GraphId tile1_id(10, 1, 0);
  const size_t tile1_size = 60000;
  cache.Put(tile1_id, graph_tile_ptr{new TestGraphTile(tile1_id, tile1_size)}, tile1_size);

  EXPECT_FALSE(cache.OverCommitted());

  GraphId tile2_id(300, 2, 0);
  const size_t tile2_size = 40000;
  cache.Put(tile2_id, graph_tile_ptr{new TestGraphTile(tile2_id, tile2_size)}, tile2_size);

  EXPECT_FALSE(cache.OverCommitted());

  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);
  CheckGraphTile(cache.Get(tile2_id), tile2_id, tile2_size);
  EXPECT_TRUE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));

  cache.Trim();

  // Not expecting any evictions if the cache does not cross the memory limit
  EXPECT_FALSE(cache.OverCommitted());
  EXPECT_TRUE(cache.Contains(tile1_id));
  EXPECT_TRUE(cache.Contains(tile2_id));
  CheckGraphTile(cache.Get(tile1_id), tile1_id, tile1_size);
  CheckGraphTile(cache.Get(tile2_id), tile2_id, tile2_size);
}

// Answers from a fixed url -> bytes map and records every request, so the remote index tests
// need neither a tile server nor real tiles.
class recording_tile_getter_t : public tile_getter_t {
public:
  struct request_t {
    std::string url;
    uint64_t offset;
    uint64_t size;
  };

  std::unordered_map<std::string, bytes_t> responses;
  std::vector<request_t> requests;

  GET_response_t get(const std::string& url, const uint64_t offset, const uint64_t size) override {
    requests.push_back({url, offset, size});
    auto found = responses.find(url);
    if (found == responses.end()) {
      return {{}, status_code_t::FAILURE, 404};
    }
    return {found->second, status_code_t::SUCCESS, 200};
  }

  HEAD_response_t head(const std::string&, header_mask_t) override {
    return {};
  }
};

constexpr size_t kIndexEntrySize = 16;
const std::string kPerTileUrl = "http://localhost/tiles/{tilePath}?token=abc";
const std::string kIndexUrl = "http://localhost/tiles/index.bin?token=abc";
const std::string kRemoteIndexTileDir = "test/gphrdr_remote_index";

// index.bin is a flat array of (offset: u64, tile_id: u32, size: u32) entries, tile_id being the
// level packed into the low 3 bits of the tile index
std::vector<char> make_index_bin(const std::vector<GraphId>& ids) {
  std::vector<char> bytes(ids.size() * kIndexEntrySize);
  char* ptr = bytes.data();
  uint64_t offset = 1024;
  for (const auto& id : ids) {
    const uint32_t tile_id = id.level() | (id.tileid() << 3);
    const uint32_t size = 2048;
    std::memcpy(ptr, &offset, sizeof(offset));
    std::memcpy(ptr + sizeof(offset), &tile_id, sizeof(tile_id));
    std::memcpy(ptr + sizeof(offset) + sizeof(tile_id), &size, sizeof(size));
    ptr += kIndexEntrySize;
    offset += size;
  }
  return bytes;
}

boost::property_tree::ptree per_tile_conf() {
  boost::property_tree::ptree pt;
  pt.put("tile_dir", kRemoteIndexTileDir);
  pt.put("tile_url", kPerTileUrl);
  return pt;
}

class RemoteTileIndex : public ::testing::Test {
protected:
  void SetUp() override {
    std::filesystem::remove_all(kRemoteIndexTileDir);
  }
  void TearDown() override {
    std::filesystem::remove_all(kRemoteIndexTileDir);
  }
};

TEST_F(RemoteTileIndex, PerTileUrlLoadsIndexBin) {
  const std::vector<GraphId> ids{{3196, 0, 0}, {51305, 1, 0}, {818660, 2, 0}};
  auto getter = std::make_unique<recording_tile_getter_t>();
  getter->responses[kIndexUrl] = make_index_bin(ids);

  GraphReader reader(per_tile_conf(), std::move(getter));

  EXPECT_EQ(reader.GetTileSet(), std::unordered_set<GraphId>(ids.begin(), ids.end()));
}

TEST_F(RemoteTileIndex, MissingIndexBinLeavesTileSetEmpty) {
  auto getter = std::make_unique<recording_tile_getter_t>();
  auto* recorder = getter.get();

  GraphReader reader(per_tile_conf(), std::move(getter));

  ASSERT_EQ(recorder->requests.size(), 1);
  EXPECT_EQ(recorder->requests[0].url, kIndexUrl);
  EXPECT_TRUE(reader.GetTileSet().empty());
}

TEST_F(RemoteTileIndex, IndexedTileIsFetchedWithoutAByteRange) {
  const GraphId id{818660, 2, 0};
  auto getter = std::make_unique<recording_tile_getter_t>();
  getter->responses[kIndexUrl] = make_index_bin({id});
  auto* recorder = getter.get();

  GraphReader reader(per_tile_conf(), std::move(getter));
  // nothing is registered for the tile itself, so this 404s rather than building a tile
  EXPECT_TRUE(reader.GetGraphTile(id) == nullptr);

  ASSERT_EQ(recorder->requests.size(), 2);
  EXPECT_EQ(recorder->requests[1].url, "http://localhost/tiles/2/000/818/660.gph?token=abc");
  EXPECT_EQ(recorder->requests[1].size, 0);
}

TEST_F(RemoteTileIndex, TileMissingFromIndexIsNeverRequested) {
  auto getter = std::make_unique<recording_tile_getter_t>();
  getter->responses[kIndexUrl] = make_index_bin({GraphId{818660, 2, 0}});
  auto* recorder = getter.get();

  GraphReader reader(per_tile_conf(), std::move(getter));
  EXPECT_TRUE(reader.GetGraphTile(GraphId{200305, 2, 0}) == nullptr);

  ASSERT_EQ(recorder->requests.size(), 1);
  EXPECT_EQ(recorder->requests[0].url, kIndexUrl);
}

// a host that answers unknown paths with a 200 page (SPA fallbacks do) must not pass for an index
TEST_F(RemoteTileIndex, IndexBinOfPartialEntriesIsIgnored) {
  auto getter = std::make_unique<recording_tile_getter_t>();
  auto bytes = make_index_bin({GraphId{818660, 2, 0}});
  bytes.push_back('\n');
  getter->responses[kIndexUrl] = bytes;

  GraphReader reader(per_tile_conf(), std::move(getter));

  EXPECT_FALSE(reader.HasRemoteTileIndex());
  EXPECT_TRUE(reader.GetTileSet().empty());
}

TEST_F(RemoteTileIndex, IndexBinWithInvalidTileIdIsIgnored) {
  const std::string html = "<!DOCTYPE html><html></html>\n\n\n\n";
  ASSERT_EQ(html.size() % kIndexEntrySize, 0);
  auto getter = std::make_unique<recording_tile_getter_t>();
  getter->responses[kIndexUrl] = std::vector<char>(html.begin(), html.end());

  GraphReader reader(per_tile_conf(), std::move(getter));

  EXPECT_FALSE(reader.HasRemoteTileIndex());
  EXPECT_TRUE(reader.GetTileSet().empty());
}

// tiles cached by an earlier run are only part of the tileset, so they must not count as an index
TEST_F(RemoteTileIndex, CachedTilesAreNotAnIndex) {
  const auto cached = std::filesystem::path(kRemoteIndexTileDir) / "2/000/818/660.gph";
  std::filesystem::create_directories(cached.parent_path());
  std::ofstream(cached) << "tile";

  GraphReader reader(per_tile_conf(), std::make_unique<recording_tile_getter_t>());

  EXPECT_FALSE(reader.GetTileSet().empty());
  EXPECT_FALSE(reader.HasRemoteTileIndex());
}

TEST_F(RemoteTileIndex, LoadedIndexBinIsReported) {
  auto getter = std::make_unique<recording_tile_getter_t>();
  getter->responses[kIndexUrl] = make_index_bin({GraphId{818660, 2, 0}});

  GraphReader reader(per_tile_conf(), std::move(getter));

  EXPECT_TRUE(reader.HasRemoteTileIndex());
}

const std::string kTarUrl = "http://localhost/tiles.tar";

boost::property_tree::ptree tar_conf() {
  boost::property_tree::ptree pt;
  pt.put("tile_url", kTarUrl);
  return pt;
}

std::string tar_url_error(const std::vector<char>& first_bytes) {
  auto getter = std::make_unique<recording_tile_getter_t>();
  getter->responses[kTarUrl] = first_bytes;
  try {
    GraphReader reader(tar_conf(), std::move(getter));
  } catch (const std::runtime_error& e) {
    return e.what();
  }
  return "";
}

TEST(RemoteTar, GzippedTarIsNamedAsSuch) {
  std::vector<char> gzipped_tar(sizeof(valhalla::midgard::tar::header_t), '\0');
  gzipped_tar[0] = '\x1f';
  gzipped_tar[1] = '\x8b';
  const auto error = tar_url_error(gzipped_tar);

  EXPECT_NE(error.find("gzipped"), std::string::npos);
  // the tar on disk may well be plain and the server compressing it on the wire, so say both
  EXPECT_NE(error.find("transport compression"), std::string::npos);
}

TEST(RemoteTar, ShortResponseIsNotReadAsATarHeader) {
  const std::string body{"<html>not a tar</html>"};
  const auto error = tar_url_error({body.begin(), body.end()});

  EXPECT_NE(error.find(std::to_string(body.size())), std::string::npos);
}

} // namespace

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
