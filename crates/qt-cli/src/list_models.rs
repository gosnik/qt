//! `--list-models [search]` (archived `run_list_models`): every model of a
//! usable provider (built-in catalog, `models.json`, Ollama instances,
//! configured and `--discover-local-models` llama.cpp models, and unless
//! offline the refreshed OpenAI / OpenRouter catalogs), as the archived
//! table.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use qt_core::ModelInfo;
use qt_core::config::AgentHome;
use qt_core::model_scope::{ModelTableRow, render_model_table};
use qt_runtime::{RunMode, Runtime, RuntimeOptions};

use crate::args::CliArgs;

pub fn run(parsed: &CliArgs, cwd: &Path, home: &AgentHome) -> i32 {
    let options = RuntimeOptions {
        cwd: cwd.to_path_buf(),
        agent_home: Some(home.root().to_path_buf()),
        mode: RunMode::Print,
        no_memory: true,
        no_extensions: true,
        offline: parsed.offline,
        startup_migrations: false,
        ..Default::default()
    };
    let runtime = match Runtime::build(options) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("Warning: {e:#}");
            return 1;
        }
    };
    let engine = runtime.engine();
    if let (Some(provider), Some(key)) = (&parsed.provider, &parsed.api_key) {
        engine.set_runtime_api_key(provider, key);
    }
    let mut capability: BTreeMap<String, (String, String)> = BTreeMap::new();
    if !parsed.local_model_dirs.is_empty() {
        let roots: Vec<PathBuf> = parsed
            .local_model_dirs
            .iter()
            .map(|d| {
                let p = PathBuf::from(d);
                if p.is_absolute() { p } else { cwd.join(p) }
            })
            .collect();
        let (found, notes) = runtime
            .llama()
            .discover_and_register(&engine.settings(), cwd, &roots);
        for note in notes {
            if note.starts_with("local model discovery could not update settings") {
                eprintln!("Warning: {note}");
            } else {
                eprintln!("Info: {note}");
            }
        }
        for model in found.models {
            capability.insert(
                model.id.clone(),
                (
                    model.capability.as_str().to_string(),
                    model.best_usage.clone(),
                ),
            );
        }
    }
    let mut models: BTreeMap<(String, String), ModelInfo> = BTreeMap::new();
    for provider in engine.providers().into_iter().filter(|p| p.is_ready()) {
        for spec in provider.models.iter() {
            let info = ModelInfo::from_spec(&provider.id, spec);
            models.insert((info.provider.clone(), info.id.clone()), info);
        }
    }
    if !parsed.offline
        && let Ok(listing) = engine.block_on(engine.list_models())
    {
        for (_, result) in listing {
            for info in result.unwrap_or_default() {
                models
                    .entry((info.provider.clone(), info.id.clone()))
                    .or_insert(info);
            }
        }
    }
    let rows: Vec<ModelTableRow> = models
        .values()
        .map(|info| {
            let mut row = ModelTableRow::from_info(info);
            if info.provider == qt_llama::PROVIDER_ID
                && let Some((cap, usage)) = capability.get(&info.id)
            {
                row.capability = cap.clone();
                row.usage = usage.clone();
            }
            row
        })
        .collect();
    print!(
        "{}",
        render_model_table(&rows, parsed.list_models.as_deref().unwrap_or(""))
    );
    0
}
