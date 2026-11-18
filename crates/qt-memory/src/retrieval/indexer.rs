//! Durable-lease indexing worker step (archived `retrieval_indexer.cpp`).

use std::io::{Read as _, Seek as _, SeekFrom, Write as _};
use std::sync::Arc;
use std::time::Duration;

use super::{Cancellation, DenseChunkVector, Embedder, RetrievalChunk, RetrievalIndexJob};
use crate::error::{MemoryError, Result};
use crate::model::{MemoryScope, RecallPolicy};
use crate::store::MemoryStore;

/// Lease length used by the indexer (renewed at every checkpoint).
const LEASE_SECONDS: i64 = 300;
const WINDOW: usize = 2048;
const OVERLAP: usize = 128;

/// Outcome of one [`RetrievalIndexer::step`].
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct RetrievalIndexStep {
    pub worked: bool,
    pub completed: bool,
    pub owner_key: String,
    pub error: String,
}

/// Waits up to the given duration for background work to become runnable;
/// `false` means still paused.
pub type IndexWait<'a> = &'a dyn Fn(Duration) -> bool;

/// Processes one durable lease at a time.
pub struct RetrievalIndexer {
    store: Arc<MemoryStore>,
    embedder: Option<Arc<dyn Embedder>>,
}

fn continuation(byte: u8) -> bool {
    byte & 0xc0 == 0x80
}

impl RetrievalIndexer {
    pub fn new(store: Arc<MemoryStore>, embedder: Option<Arc<dyn Embedder>>) -> Self {
        Self { store, embedder }
    }

    /// Splits text into 2 KiB windows with 128-byte overlap, preferring line
    /// breaks in the second half of a window and never splitting UTF-8.
    pub fn render(text: &str) -> Result<Vec<RetrievalChunk>> {
        if text.is_empty() {
            return Err(MemoryError::invalid("cannot index empty content"));
        }
        if text.len() > 1024 * 1024 {
            return Err(MemoryError::invalid("index owner content exceeds one MiB"));
        }
        let bytes = text.as_bytes();
        let mut chunks = Vec::new();
        let mut begin = 0usize;
        while begin < bytes.len() {
            let mut end = bytes.len().min(begin + WINDOW);
            if end < bytes.len() {
                while end > begin && continuation(bytes[end]) {
                    end -= 1;
                }
                if end > begin
                    && let Some(newline) = bytes[..end].iter().rposition(|byte| *byte == b'\n')
                    && newline > begin + WINDOW / 2
                {
                    end = newline + 1;
                }
            }
            if end <= begin {
                return Err(MemoryError::invalid("unable to partition malformed text"));
            }
            chunks.push(RetrievalChunk {
                ordinal: chunks.len(),
                text: text[begin..end].to_string(),
                source_span: format!("bytes:{begin}:{end}"),
                ..Default::default()
            });
            if end == bytes.len() {
                break;
            }
            begin = if end > begin + OVERLAP {
                end - OVERLAP
            } else {
                end
            };
            while begin < end && continuation(bytes[begin]) {
                begin += 1;
            }
        }
        Ok(chunks)
    }

    /// Claims and processes one job of exactly `scope`. Failures are recorded
    /// on the job (retry with backoff) and reported in the step.
    pub fn step(
        &self,
        scope: &MemoryScope,
        cancelled: Cancellation<'_>,
        dense_generation: Option<&str>,
        wait_until_ready: Option<IndexWait<'_>>,
    ) -> Result<RetrievalIndexStep> {
        let mut result = RetrievalIndexStep::default();
        if cancelled() {
            return Ok(result);
        }
        let Some(job) = self
            .store
            .claim_index_job(scope, LEASE_SECONDS, dense_generation)?
        else {
            return Ok(result);
        };
        result.worked = true;
        result.owner_key = job.owner_key.clone();
        match self.process(scope, &job, cancelled, wait_until_ready) {
            Ok(completed) => {
                result.completed = completed;
                if !completed {
                    result.error = "indexing result superseded".into();
                }
            }
            Err(error) => {
                result.error = error.to_string();
                let _ = self.store.fail_index_job(scope, &job, &result.error, 5);
            }
        }
        Ok(result)
    }

    fn checkpoint(
        &self,
        scope: &MemoryScope,
        job: &RetrievalIndexJob,
        cancelled: Cancellation<'_>,
        wait_until_ready: Option<IndexWait<'_>>,
    ) -> Result<()> {
        loop {
            if cancelled() {
                return Err(MemoryError::storage("indexing cancelled"));
            }
            if !self.store.renew_index_job(scope, job, LEASE_SECONDS)? {
                return Err(MemoryError::storage("indexing lease superseded"));
            }
            match wait_until_ready {
                Some(wait) if !wait(Duration::from_secs(1)) => continue,
                _ => break,
            }
        }
        if cancelled() {
            return Err(MemoryError::storage("indexing cancelled"));
        }
        Ok(())
    }

    fn process(
        &self,
        scope: &MemoryScope,
        job: &RetrievalIndexJob,
        cancelled: Cancellation<'_>,
        wait_until_ready: Option<IndexWait<'_>>,
    ) -> Result<bool> {
        self.checkpoint(scope, job, cancelled, wait_until_ready)?;
        match job.representation.as_str() {
            "chunks" => {
                let chunks = Self::render(&job.content)?;
                self.checkpoint(scope, job, cancelled, wait_until_ready)?;
                self.store.publish_chunks(scope, job, chunks)
            }
            "dense" => {
                let Some(embedder) = &self.embedder else {
                    return Err(MemoryError::storage("embedding provider unavailable"));
                };
                let config = self
                    .store
                    .dense_index(scope, &job.generation, RecallPolicy::Strict, false)?
                    .ok_or_else(|| MemoryError::storage("embedding generation unavailable"))?;
                if config.dimensions == 0 || config.dimensions > 65536 {
                    return Err(MemoryError::storage(
                        "embedding dimension exceeds indexing bounds",
                    ));
                }
                let chunks = self.store.retrieval_chunks(scope, &job.owner_key)?;
                if chunks.is_empty() || chunks.len() > 4096 {
                    return Err(MemoryError::storage(
                        "dense chunk count exceeds indexing bounds",
                    ));
                }
                // Keep one vector in memory at a time; the anonymous spool is
                // discarded on failure and the durable job is the recovery
                // authority.
                let mut spool = tempfile::tempfile()
                    .map_err(|_| MemoryError::storage("unable to create dense indexing spool"))?;
                for chunk in &chunks {
                    self.checkpoint(scope, job, cancelled, wait_until_ready)?;
                    let values = embedder.embed(
                        &config,
                        &format!("{}{}", config.document_prefix, chunk.text),
                        cancelled,
                    )?;
                    if values.len() != config.dimensions {
                        return Err(MemoryError::storage(
                            "embedding provider returned the wrong dimension",
                        ));
                    }
                    let bytes: Vec<u8> = values
                        .iter()
                        .flat_map(|value| value.to_le_bytes())
                        .collect();
                    spool.write_all(&bytes).map_err(|_| {
                        MemoryError::storage("unable to write dense indexing spool")
                    })?;
                }
                self.checkpoint(scope, job, cancelled, wait_until_ready)?;
                spool
                    .flush()
                    .and_then(|()| spool.seek(SeekFrom::Start(0)).map(|_| ()))
                    .map_err(|_| MemoryError::storage("unable to rewind dense indexing spool"))?;
                let mut next = 0usize;
                let dimensions = config.dimensions;
                let mut read_next = || -> Result<DenseChunkVector> {
                    if cancelled() {
                        return Err(MemoryError::storage("indexing cancelled"));
                    }
                    let chunk = chunks.get(next).ok_or_else(|| {
                        MemoryError::storage("unable to read dense indexing spool")
                    })?;
                    next += 1;
                    let mut bytes = vec![0u8; dimensions * 4];
                    spool
                        .read_exact(&mut bytes)
                        .map_err(|_| MemoryError::storage("unable to read dense indexing spool"))?;
                    Ok(DenseChunkVector {
                        chunk_id: chunk.id.clone(),
                        values: bytes
                            .as_chunks::<4>()
                            .0
                            .iter()
                            .map(|item| f32::from_le_bytes(*item))
                            .collect(),
                    })
                };
                let completed =
                    self.store
                        .publish_dense_vectors(scope, job, chunks.len(), &mut read_next)?;
                if completed {
                    let _ = self.store.activate_dense_index(scope, &job.generation)?;
                }
                Ok(completed)
            }
            _ => Err(MemoryError::storage("unsupported retrieval representation")),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn render_windows_with_overlap_and_utf8_boundaries() {
        assert!(RetrievalIndexer::render("").is_err());
        let short = RetrievalIndexer::render("hello").unwrap();
        assert_eq!(short.len(), 1);
        assert_eq!(short[0].source_span, "bytes:0:5");
        let text = "é".repeat(3000);
        let chunks = RetrievalIndexer::render(&text).unwrap();
        assert!(chunks.len() > 1);
        assert_eq!(chunks[0].source_span, "bytes:0:2048");
        assert_eq!(chunks[1].source_span, "bytes:1920:3968");
        let lines = format!("{}\n{}", "a".repeat(1500), "b".repeat(2000));
        let chunks = RetrievalIndexer::render(&lines).unwrap();
        assert_eq!(chunks[0].source_span, "bytes:0:1501");
        assert_eq!(chunks[1].source_span, "bytes:1373:3421");
    }
}
