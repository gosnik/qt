// Load with: qt -e examples/extensions/astra-auto-compact.as
// Native compaction is synchronous and idle-only: check before the next prompt.
const int64 ASTRA_COMPACT_THRESHOLD = 200000;

void activate() {
  qt_on("input", "astra_auto_compact_input");
}

bool astra_matches(const string &in encoded_name) {
  if (encoded_name == "null") return false;
  string name = qt_json_string(encoded_name);
  for (uint i = 0; i < name.length(); ++i) {
    if (name[i] >= 65 && name[i] <= 90) name[i] += 32;
  }
  return name.findFirst("astra") >= 0;
}

string astra_auto_compact_input(const string &in event_json) {
  const string unchanged = "{\"action\":\"continue\"}";
  if (qt_host_call("ctx.isIdle", "{}") != "true") return unchanged;

  string model = qt_host_call("ctx.getModel", "{}");
  if (model == "null") return unchanged;
  if (!astra_matches(qt_json_get(model, "id")) &&
      !astra_matches(qt_json_get(model, "name"))) return unchanged;

  string usage = qt_host_call("ctx.getContextUsage", "{}");
  if (usage == "null") return unchanged;
  string tokens = qt_json_get(usage, "tokens");
  if (tokens == "null") return unchanged;
  uint parsed = 0;
  int64 count = parseInt(tokens, 10, parsed);
  if (parsed != tokens.length() || count <= ASTRA_COMPACT_THRESHOLD)
    return unchanged;

  try {
    qt_host_call("ctx.compact", "{}");
  } catch {
    qt_host_call("ctx.ui.notify",
        "{\"message\":\"Astra auto-compaction failed; continuing with the existing context.\","
        "\"notifyType\":\"warning\"}");
  }
  return unchanged;
}
