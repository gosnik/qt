#include "qt/ai/openai_model_discovery.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

namespace {

class FakeTransport final : public qt::ai::HttpTransport {
 public:
  qt::ai::HttpResponse response;
  qt::ai::HttpRequest last_request;
  bool throw_error = false;

  qt::ai::HttpResponse send(const qt::ai::HttpRequest& request) override {
    last_request = request;
    if (throw_error) {
      throw std::runtime_error("secret transport detail");
    }
    return response;
  }
};

qt::ai::ModelCatalog known_catalog() {
  qt::ai::ModelCatalog catalog;
  qt::ai::ModelCatalogProviderRecord provider;
  provider.descriptor.id = "openai";
  provider.descriptor.api = "openai-responses";
  provider.metadata = qt::ai::Json{{"baseUrl", "https://api.openai.com/v1"},
                                   {"headers", qt::ai::Json{{"X-Project", "p1"}}},
                                   {"custom", "kept"}};
  catalog.providers.push_back(provider);

  qt::ai::ModelCatalogModelRecord astra;
  astra.ref = {"openai", "openai-responses", "gpt-astra"};
  astra.display_name = "Astra custom";
  astra.capabilities.reasoning = true;
  astra.metadata = qt::ai::Json{{"input", {"text", "image"}},
                                {"cost", qt::ai::Json{{"input", 1.0}}},
                                {"contextWindow", 999},
                                {"custom", "preserve"}};
  catalog.models.push_back(astra);
  auto fine_tuned = astra;
  fine_tuned.ref.name = "ft:custom-model";
  fine_tuned.display_name = "Fine tuned custom";
  catalog.models.push_back(fine_tuned);
  return catalog;
}

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() {
  const auto known = known_catalog();
  const qt::ai::Json response = {
      {"data", {{{"id", "gpt-new"}}, {{"id", "gpt-new"}},
                 {{"id", "gpt-astra"}}, {{"id", " ft:custom-model "}},
                 {{"id", "gpt-image-1"}},
                 {{"id", "gpt-4o-realtime-preview"}},
                 {{"id", "o3-mini"}}, {{"id", "chatgpt-latest"}},
                 {{"id", "ft:gpt-4o:org:model:1"}},
                 {{"id", "ft:gpt-4o-realtime:org:model:2"}},
                 {{"id", "text-embedding-3-small"}},
                 {{"id", 7}}}}};
  const auto parsed = qt::ai::model_catalog_from_openai_response(response, known);
  require(parsed.ok(), "response parse failed");
  require(parsed.value().providers.size() == 1, "provider missing");
  require(parsed.value().models.size() == 6, "filter or duplicate handling failed");
  const auto* astra = qt::ai::find_model_record(parsed.value(), "openai", "gpt-astra");
  require(astra != nullptr && astra->metadata == known.models.front().metadata,
          "known metadata was not preserved");
  const auto* fresh = qt::ai::find_model_record(parsed.value(), "openai", "gpt-new");
  require(fresh != nullptr && fresh->metadata.at("openaiDiscovery") == true,
          "new model metadata missing discovery marker");
  require(qt::ai::find_model_record(parsed.value(), "openai",
                                    "ft:gpt-4o:org:model:1") != nullptr,
          "valid fine-tuned chat model was filtered");

  auto base = known;
  base.models.push_back(fresh ? *fresh : qt::ai::ModelCatalogModelRecord{});
  const auto replaced = qt::ai::replace_openai_models(base, parsed.value(), known);
  require(qt::ai::find_model_record(replaced, "openai", "gpt-new") == nullptr,
          "disappeared discovered model was retained");
  require(qt::ai::find_model_record(replaced, "openai", "gpt-astra") != nullptr,
          "known model was removed");
  auto modified = base;
  modified.models.back().metadata["modified"] = true;
  const auto retained_override =
      qt::ai::replace_openai_models(modified, parsed.value(), parsed.value());
  require(qt::ai::find_model_record(retained_override, "openai", "gpt-new")
              ->metadata.value("modified", false) == true,
          "modified current discovery was overwritten");

  FakeTransport transport;
  transport.response.body = response.dump();
  const auto fetched = qt::ai::discover_openai_models(
      transport, known, "api-secret", "https://example.test/v1/",
      {{"OpenAI-Organization", "org-test"}});
  require(fetched.ok(), "network discovery failed");
  require(transport.last_request.method == "GET" &&
              transport.last_request.url == "https://example.test/v1/models" &&
              transport.last_request.timeout_ms == 10'000,
          "request shape incorrect");
  require(transport.last_request.headers.at("Authorization") == "Bearer api-secret" &&
              transport.last_request.headers.at("OpenAI-Organization") == "org-test",
          "request headers incorrect");
  require(fetched.value().models.front().metadata.at("baseUrl") ==
              "https://api.openai.com/v1",
          "provider endpoint metadata was not inherited");

  require(!qt::ai::discover_openai_models(transport, known, "").ok(),
          "missing credentials accepted");
  transport.response.status = 401;
  const auto unauthorized = qt::ai::discover_openai_models(transport, known, "secret");
  require(!unauthorized.ok() && unauthorized.error().message.find("secret") ==
              std::string::npos,
          "HTTP error leaked credentials");
  transport.throw_error = true;
  const auto failed = qt::ai::discover_openai_models(transport, known, "secret");
  require(!failed.ok() && failed.error().message.find("secret") == std::string::npos,
          "transport error leaked details");
  std::cout << "openai discovery probe passed\n";
}
