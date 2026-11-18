// Load with: qt -e examples/extensions/astra-auto-compact.rhai
//
// Compacts the session before the next idle input when the current model is
// an "astra" model (model id or display name, ASCII case-insensitive) and
// the context holds more than 200000 tokens. ctx::compact() is synchronous
// and idle-only, so the check runs in the `input` hook rather than on
// turn_end. The input always continues unchanged; a failed compaction is
// reported and the prompt proceeds with the existing context.

const ASTRA_COMPACT_THRESHOLD = 200000;

fn activate() {
    on("input", "astra_auto_compact_input");
}

fn ascii_lower(text) {
    let out = "";
    for ch in text.chars() {
        let code = ch.to_int();
        out += if code >= 65 && code <= 90 { ch.to_lower() } else { ch };
    }
    out
}

fn astra_matches(name) {
    if type_of(name) != "string" {
        return false;
    }
    ascii_lower(name).contains("astra")
}

fn astra_auto_compact_input(event) {
    let unchanged = #{ action: "continue" };
    if !ctx::isIdle() {
        return unchanged;
    }

    let model = ctx::getModel();
    if model == () {
        return unchanged;
    }
    if !astra_matches(model.id) && !astra_matches(model.name) {
        return unchanged;
    }

    let usage = ctx::getContextUsage();
    if usage == () || usage.tokens == () || type_of(usage.tokens) != "i64" {
        return unchanged;
    }
    if usage.tokens <= global::ASTRA_COMPACT_THRESHOLD {
        return unchanged;
    }

    try {
        ctx::compact(#{});
    } catch {
        ctx::ui::notify(#{
            message: "Astra auto-compaction failed; continuing with the existing context.",
            notifyType: "warning",
        });
    }
    unchanged
}
