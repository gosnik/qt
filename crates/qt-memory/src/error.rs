//! Error type shared by the memory store and services.
//!
//! The archived C++ implementation distinguishes `std::invalid_argument`
//! (caller or state errors that surfaces may report verbatim) from
//! `std::runtime_error` (storage failures). The same split is kept here, and the
//! message texts match the archived wording because callers branch on some of
//! them (for example "review item not found in scope").

use thiserror::Error;

#[derive(Debug, Error)]
pub enum MemoryError {
    /// Rejected input or a state precondition that does not hold.
    #[error("{0}")]
    InvalidArgument(String),
    /// Storage or I/O failure.
    #[error("{0}")]
    Storage(String),
}

impl MemoryError {
    pub fn invalid(message: impl Into<String>) -> Self {
        Self::InvalidArgument(message.into())
    }

    pub fn storage(message: impl Into<String>) -> Self {
        Self::Storage(message.into())
    }

    /// True for argument/state errors (the archived `std::invalid_argument`).
    pub fn is_invalid_argument(&self) -> bool {
        matches!(self, Self::InvalidArgument(_))
    }

    pub fn message(&self) -> &str {
        match self {
            Self::InvalidArgument(message) | Self::Storage(message) => message,
        }
    }
}

impl From<rusqlite::Error> for MemoryError {
    fn from(error: rusqlite::Error) -> Self {
        Self::Storage(format!("memory sqlite execution failed: {error}"))
    }
}

impl From<std::io::Error> for MemoryError {
    fn from(error: std::io::Error) -> Self {
        Self::Storage(error.to_string())
    }
}

pub type Result<T, E = MemoryError> = std::result::Result<T, E>;

pub(crate) fn invalid<T>(message: impl Into<String>) -> Result<T> {
    Err(MemoryError::invalid(message))
}

pub(crate) fn storage<T>(message: impl Into<String>) -> Result<T> {
    Err(MemoryError::storage(message))
}
