#include "qt/ai/ollama.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace qt::ai {
HttpResponse BeastHttpTransport::send(const HttpRequest&) {
  throw std::runtime_error("unexpected live HTTP request in Ollama probe");
}

ModelCatalog normalized(const ModelCatalog& catalog) { return catalog; }
}  // namespace qt::ai

namespace {

class FakeTransport final : public qt::ai::HttpTransport {
 public:
  qt::ai::HttpResponse send(const qt::ai::HttpRequest& request) override {
    requests.push_back(request);
    if (responses.empty()) {
      throw std::runtime_error("probe response queue exhausted");
    }
    auto response = std::move(responses.front());
    responses.erase(responses.begin());
    return response;
  }

  std::vector<qt::ai::HttpResponse> responses;
  std::vector<qt::ai::HttpRequest> requests;
};

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

}  // namespace

int main() {
  try {
    require(qt::ai::normalize_ollama_base_url(" http://host:11434/v1/ ") ==
                "http://host:11434",
            "base URL normalization failed");

    qt::ai::OllamaInstanceSettings first;
    first.id = "desk";
    first.name = "Desk";
    first.base_url = "http://desk:11434/";
    first.api_key = "secret";
    FakeTransport transport;
    transport.responses = {
        {200, {}, R"({"models":[{"name":"team/model:7b","digest":"abc","size":42}]})"},
        {200, {}, R"({"capabilities":["completion","tools","vision","thinking"],"model_info":{"llama.context_length":65536},"details":{"family":"llama"}})"},
    };
    const auto discovered = qt::ai::discover_ollama_models(first, &transport);
    require(discovered.ok(), "model discovery failed");
    require(discovered.value().size() == 1U, "unexpected model count");
    const auto& model = discovered.value().front();
    require(model.name == "team/model:7b", "model name was changed");
    require(model.context_window == 65536, "context metadata was not read");
    require(model.max_tokens == qt::ai::kDefaultOllamaMaxTokens,
            "max output token cap was not applied");
    require(transport.requests.size() == 2U &&
                transport.requests[0].url == "http://desk:11434/api/tags" &&
                transport.requests[1].url == "http://desk:11434/api/show",
            "discovery endpoint routing failed");
    require(transport.requests[0].headers.at("Authorization") == "Bearer secret",
            "instance credential was not isolated on request");

    first.models = discovered.value();
    auto second = first;
    second.id = "gpu";
    second.name = "GPU";
    second.base_url = "https://gpu.example/v1";
    second.api_key.reset();
    const auto catalog =
        qt::ai::model_catalog_from_ollama_settings({first, second});
    require(catalog.providers.size() == 2U && catalog.models.size() == 2U,
            "same model on two instances was collapsed");
    require(catalog.providers[0].descriptor.api == "openai-completions",
            "Ollama provider API family is incorrect");
    require(catalog.models[0].ref.provider != catalog.models[1].ref.provider &&
                catalog.models[0].ref.name == catalog.models[1].ref.name,
            "instance identity leaked into model identity");
    require(catalog.providers[1].metadata.at("baseUrl") ==
                "https://gpu.example/v1",
            "OpenAI compatibility endpoint was not derived correctly");
    require(catalog.models[1].metadata.at("timeoutMs") ==
                qt::ai::kDefaultOllamaTimeoutMs,
            "instance timeout was not carried into model metadata");

    FakeTransport failed;
    failed.responses.push_back({503, {}, R"({"error":"warming up"})"});
    const auto failure = qt::ai::ollama_request(
        second, "GET", "/api/version", qt::ai::Json(), &failed);
    require(!failure && failure.error().message.find("warming up") != std::string::npos,
            "structured server error was not preserved");

    std::cout << "ollama_probe: ok\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "ollama_probe: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
