#include "qt/memory/database.hpp"
#include "qt/memory/recall_service.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace qt::memory;

namespace {

void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}

MemoryScope scope_for(const std::string& name) {
  MemoryScope scope;
  scope.kind = name == "global" ? ScopeKind::GlobalUser : ScopeKind::Repository;
  scope.user = "memory-v2-baseline-user";
  if (name != "global") {
    scope.project = "qtbits";
    scope.repository = name == "alpha" ? "qt" : "other-repository";
  }
  return scope;
}

MemoryStatus status_for(const std::string& value) {
  if (value == "validated") return MemoryStatus::Validated;
  if (value == "active") return MemoryStatus::Active;
  if (value == "candidate") return MemoryStatus::Candidate;
  if (value == "hypothesis") return MemoryStatus::Hypothesis;
  if (value == "superseded") return MemoryStatus::Superseded;
  if (value == "deprecated") return MemoryStatus::Deprecated;
  throw std::runtime_error("unknown fixture status: " + value);
}

Json load_json(const fs::path& path) {
  std::ifstream input(path);
  require(static_cast<bool>(input), "unable to open fixture: " + path.string());
  return Json::parse(input);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 3, "usage: memory_v2_baseline_probe <fixture.json> <agent-home>");
    const Json fixture = load_json(argv[1]);
    fs::remove_all(argv[2]);
    fs::create_directories(argv[2]);
    SQLiteMemoryDatabase database(argv[2]);
    std::map<std::string, std::string> actual_ids;
    std::map<std::string, std::string> labels_by_id;
    for (const auto& item : fixture.at("memories")) {
      MemoryWrite write;
      write.status = status_for(item.at("status"));
      write.type = MemoryType::Decision;
      write.content = item.at("content");
      write.source = "memory-v2-baseline";
      write.confidence = item.value("confidence", 0.5);
      write.quality_score = item.value("quality", 0.0);
      write.provenance.source_type = SourceType::Manual;
      write.provenance.source_uri = "test://memory-v2/" + item.at("id").get<std::string>();
      write.provenance.source_hash = "fixture:" + item.at("id").get<std::string>();
      const auto actual = database.add(scope_for(item.at("scope")), std::move(write));
      actual_ids[item.at("id")] = actual;
      labels_by_id[actual] = item.at("id");
    }

    RecallService recall(database);
    Json cases = Json::array();
    std::size_t hit1 = 0, hit3 = 0, hit5 = 0, forbidden = 0;
    double mrr_sum = 0.0;
    double recall1_sum = 0.0, recall3_sum = 0.0, recall5_sum = 0.0;
    for (const auto& item : fixture.at("queries")) {
      RecallRequest request;
      request.query = item.at("query");
      request.policy = RecallPolicy::Broad;
      request.include_global_user = item.at("scope") == "alpha" &&
                                    item.at("id") == "security-scope";
      request.candidate_limit = 50;
      request.result_limit = 20;
      const auto results = recall.recall(scope_for(item.at("scope")), request);
      std::set<std::string> relevant;
      for (const auto& label : item.at("relevant")) relevant.insert(label);
      std::set<std::string> forbidden_labels;
      for (const auto& label : item.value("forbidden", Json::array()))
        forbidden_labels.insert(label);
      Json ranking = Json::array();
      std::size_t first_relevant = 0;
      std::size_t relevant1 = 0, relevant3 = 0, relevant5 = 0;
      std::set<std::string> seen_relevant;
      bool forbidden_hit = false;
      for (std::size_t rank = 0; rank < results.size(); ++rank) {
        const auto found = labels_by_id.find(results[rank].memory.id);
        const auto label = found == labels_by_id.end() ? results[rank].memory.id : found->second;
        const bool is_relevant = relevant.count(label) != 0;
        const bool is_forbidden = forbidden_labels.count(label) != 0;
        if (is_relevant && first_relevant == 0) first_relevant = rank + 1;
        if (is_relevant && seen_relevant.insert(label).second) {
          if (rank < 1) ++relevant1;
          if (rank < 3) ++relevant3;
          if (rank < 5) ++relevant5;
        }
        forbidden_hit = forbidden_hit || is_forbidden;
        ranking.push_back({{"rank", rank + 1}, {"label", label},
                           {"score", results[rank].score.total},
                           {"eligible_for_context", results[rank].eligible_for_context}});
      }
      if (first_relevant == 1) ++hit1;
      if (first_relevant != 0 && first_relevant <= 3) ++hit3;
      if (first_relevant != 0 && first_relevant <= 5) ++hit5;
      if (first_relevant != 0) mrr_sum += 1.0 / static_cast<double>(first_relevant);
      if (forbidden_hit) ++forbidden;
      require(!relevant.empty(), "each evaluation query needs relevance labels");
      recall1_sum += static_cast<double>(relevant1) / relevant.size();
      recall3_sum += static_cast<double>(relevant3) / relevant.size();
      recall5_sum += static_cast<double>(relevant5) / relevant.size();
      cases.push_back({{"id", item.at("id")}, {"query", request.query},
                       {"first_relevant_rank", first_relevant},
                       {"forbidden_hit", forbidden_hit}, {"ranking", ranking}});
    }
    const auto count = cases.size();
    Json report{{"version", 2},
                {"fixture", fs::absolute(argv[1]).string()},
                {"retriever", "production RecallService"},
                {"case_count", count},
                {"metrics", {{"recall_at_1", count ? recall1_sum / count : 0.0},
                              {"recall_at_3", count ? recall3_sum / count : 0.0},
                              {"recall_at_5", count ? recall5_sum / count : 0.0},
                              {"hit_at_1", count ? static_cast<double>(hit1) / count : 0.0},
                              {"hit_at_3", count ? static_cast<double>(hit3) / count : 0.0},
                              {"hit_at_5", count ? static_cast<double>(hit5) / count : 0.0},
                              {"mrr", count ? mrr_sum / count : 0.0},
                              {"forbidden_hit_rate", count ? static_cast<double>(forbidden) / count : 0.0}}},
                {"cases", cases}};
    std::cout << report.dump() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "memory v2 baseline probe failed: " << error.what() << '\n';
    return 1;
  }
}
