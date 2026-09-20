#pragma once

#include <cstddef>

namespace qt::app::generated {

struct EmbeddedAsset {
    const char* path;
    const char* content_type;
    const unsigned char* data;
    std::size_t size;
    std::size_t uncompressed_size;
    bool gzip_encoded;
};

const EmbeddedAsset* find_dashboard_asset(const char* path);
std::size_t dashboard_asset_count();
bool dashboard_assets_available();

}  // namespace qt::app::generated
