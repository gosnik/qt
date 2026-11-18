//! Extensions in the desktop app: the GPUI `ExtensionUi` (`ctx.ui.*`
//! dialogs, status, widgets, header and footer, working indicator, editor
//! text, themes, components, autocomplete providers), registered shortcuts,
//! message renderers, extension commands run off the UI thread, the
//! package resource selectors and `/reload`.

pub mod bridge;
pub mod chrome;
pub mod dialogs;
pub mod keys;
pub mod packages;

pub use chrome::{ChromeEvent, ExtensionChrome, prefix_start};
pub use packages::open_packages;
