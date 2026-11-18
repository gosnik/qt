//! Items for the model and thinking-level selectors.

use gpui_kit::component::searchable_list::SearchableListItem;
use gpui_kit::*;
use qt_core::provider::{ModelInfo, ModelRef, ThinkingLevel};

/// One selectable model; the value is the `provider/id` spec.
#[derive(Clone, Debug, PartialEq)]
pub struct ModelItem {
    pub title: SharedString,
    pub value: SharedString,
    pub context_window: Option<u64>,
}

impl ModelItem {
    pub fn from_info(info: &ModelInfo, provider_label: &str) -> Self {
        let name = if info.name == info.id {
            info.id.clone()
        } else {
            format!("{} ({})", info.name, info.id)
        };
        Self {
            title: format!("{provider_label} · {name}").into(),
            value: info.model_ref().to_string().into(),
            context_window: info.context_window,
        }
    }

    /// Placeholder for a model that is selected but not (yet) listed.
    pub fn from_ref(model: &ModelRef) -> Self {
        Self {
            title: format!("{} · {}", model.provider, model.id).into(),
            value: model.to_string().into(),
            context_window: None,
        }
    }
}

impl SearchableListItem for ModelItem {
    type Value = SharedString;

    fn title(&self) -> SharedString {
        self.title.clone()
    }

    fn value(&self) -> &SharedString {
        &self.value
    }

    fn matches(&self, query: &str) -> bool {
        let query = query.to_lowercase();
        query.split_whitespace().all(|word| {
            self.title.to_lowercase().contains(word) || self.value.to_lowercase().contains(word)
        })
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct ThinkingItem {
    pub title: SharedString,
    pub value: SharedString,
}

impl ThinkingItem {
    pub fn all() -> Vec<Self> {
        ThinkingLevel::ALL
            .iter()
            .map(|level| {
                let title = match level {
                    ThinkingLevel::Off => "Thinking: off",
                    ThinkingLevel::Minimal => "Thinking: minimal",
                    ThinkingLevel::Low => "Thinking: low",
                    ThinkingLevel::Medium => "Thinking: medium",
                    ThinkingLevel::High => "Thinking: high",
                    ThinkingLevel::XHigh => "Thinking: xhigh",
                };
                Self {
                    title: title.into(),
                    value: level.as_str().into(),
                }
            })
            .collect()
    }
}

impl SearchableListItem for ThinkingItem {
    type Value = SharedString;

    fn title(&self) -> SharedString {
        self.title.clone()
    }

    fn value(&self) -> &SharedString {
        &self.value
    }
}
