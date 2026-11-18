// Load with: qt -e examples/extensions/compact-command.rhai
//
// `/squash [instructions]` compacts the idle session from a command, passing
// any argument text as custom summary instructions. The extension also
// observes its own compaction: `session_before_compact` and `session_compact`
// run while ctx::compact() waits (on the command's thread), so the command
// can report what its hooks saw. Compactions started elsewhere (`/compact`,
// threshold or overflow recovery) are reported through a notification.

fn activate() {
    this.pending = false;
    this.reason = "";
    this.tokens_before = 0;
    registerCommand("squash", "Squash", "Compact the session now (optional instructions)");
    on("session_before_compact", "squash_before_compact");
    on("session_compact", "squash_compacted");
}

fn squash_before_compact(event) {
    this.reason = event.reason;
    this.instructions = event.customInstructions;
    ()
}

fn squash_compacted(event) {
    let entry = event.compactionEntry;
    this.tokens_before = entry.tokensBefore;
    if !this.pending {
        ctx::ui::notify(#{
            message: `Session compacted (${this.reason}, ${entry.tokensBefore} tokens before).`,
            notifyType: "info",
        });
    }
}

fn command_squash(args) {
    if !ctx::isIdle() {
        return "Wait for the agent to finish before /squash.";
    }
    // trim() changes the string in place.
    let instructions = args.args;
    instructions.trim();
    let custom = if instructions.is_empty() { () } else { instructions };
    this.pending = true;
    this.reason = "";
    let text = "";
    try {
        let result = ctx::compact(#{ customInstructions: custom });
        text = `Compacted (${this.reason}): ${result.tokensBefore} tokens summarized, kept from ${result.firstKeptEntryId}.`;
    } catch (e) {
        text = `Compaction failed: ${e}`;
    }
    this.pending = false;
    text
}
