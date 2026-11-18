#pragma once

#include <optional>
#include <string>
#include <vector>

#include "qt/ai/models.hpp"
#include "qt/ai/provider.hpp"
#include "qt/foundation/error.hpp"

namespace qt::ai {

struct ModelCatalogProviderRecord {
  ProviderDescriptor descriptor;
  Json metadata = Json::object();
};

struct ModelCatalogModelRecord {
  ModelRef ref;
  std::string display_name;
  ProviderCapabilities capabilities;
  Json metadata = Json::object();
};

struct ModelCatalog {
  std::vector<ModelCatalogProviderRecord> providers;
  std::vector<ModelCatalogModelRecord> models;
};

struct ImageModelCatalogRecord {
  std::string provider;
  std::string api;
  std::string id;
  std::string display_name;
  std::string base_url;
  std::vector<std::string> input;
  std::vector<std::string> output;
  Json metadata = Json::object();
};

struct ParsedModelCatalogReference {
  std::optional<std::string> provider;
  std::string model;
};

ModelCatalogProviderRecord normalized(const ModelCatalogProviderRecord& record);
ModelCatalogModelRecord normalized(const ModelCatalogModelRecord& record);
ModelCatalog normalized(const ModelCatalog& catalog);
ImageModelCatalogRecord normalized(const ImageModelCatalogRecord& record);
ModelCatalog default_model_catalog();
std::vector<ImageModelCatalogRecord> default_image_model_catalog();

bool is_valid(const ModelCatalogProviderRecord& record);
bool is_valid(const ModelCatalogModelRecord& record);
bool is_valid(const ModelCatalog& catalog);

std::vector<std::string> validate(const ModelCatalogProviderRecord& record);
std::vector<std::string> validate(const ModelCatalogModelRecord& record);
std::vector<std::string> validate(const ModelCatalog& catalog);

ParsedModelCatalogReference parse_model_catalog_reference(
    const std::string& reference);

const ModelCatalogProviderRecord* find_provider_record(
    const ModelCatalog& catalog, const std::string& provider_id);
const ModelCatalogModelRecord* find_model_record(
    const ModelCatalog& catalog, const std::string& provider_id,
    const std::string& model_id);
const ModelCatalogModelRecord* match_model_record(
    const ModelCatalog& catalog, const std::string& reference);
const ImageModelCatalogRecord* find_image_model_record(
    const std::vector<ImageModelCatalogRecord>& catalog,
    const std::string& provider_id, const std::string& model_id);
std::vector<std::string> supported_thinking_levels(
    const ModelCatalogModelRecord& record);
std::string clamp_thinking_level(const ModelCatalogModelRecord& record,
                                 const std::string& level);

ModelCatalog merge_catalogs(const ModelCatalog& base, const ModelCatalog& overlay);
qt::foundation::Result<ModelCatalog> model_catalog_from_provider_config(
    const std::string& provider_id, const Json& config,
    const ModelCatalog& base_catalog);
qt::foundation::Result<ModelCatalog> apply_provider_config_to_model_catalog(
    const ModelCatalog& base_catalog, const std::string& provider_id,
    const Json& config);
qt::foundation::Result<ModelCatalog> apply_models_config_to_model_catalog(
    const ModelCatalog& base_catalog, const Json& config);
std::optional<TokenCostRates> token_cost_rates_from_model_metadata(
    const ModelCatalogModelRecord& record);
ProviderOptions provider_options_from_model_metadata(
    const ModelCatalogModelRecord& record);
std::optional<TokenCostRates> token_cost_rates_from_image_model_metadata(
    const ImageModelCatalogRecord& record);
ProviderOptions provider_options_from_image_model_metadata(
    const ImageModelCatalogRecord& record);

}  // namespace qt::ai
