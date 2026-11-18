//! Minimal GGUF metadata reader (header and key/value section only).
//!
//! Mirrors the archived scanner: scalar integers, booleans and strings are
//! kept; floats and arrays are skipped; bounded entry, string and array sizes
//! protect against corrupt files. Keys are returned sorted (the archived
//! implementation stored them in an ordered JSON object).

use std::collections::BTreeMap;
use std::fs::File;
use std::io::{BufReader, Read, Seek, SeekFrom};
use std::path::Path;

use serde_json::{Map, Value};

const TYPE_UINT8: u32 = 0;
const TYPE_INT8: u32 = 1;
const TYPE_UINT16: u32 = 2;
const TYPE_INT16: u32 = 3;
const TYPE_UINT32: u32 = 4;
const TYPE_INT32: u32 = 5;
const TYPE_FLOAT32: u32 = 6;
const TYPE_BOOL: u32 = 7;
const TYPE_STRING: u32 = 8;
const TYPE_ARRAY: u32 = 9;
const TYPE_UINT64: u32 = 10;
const TYPE_INT64: u32 = 11;
const TYPE_FLOAT64: u32 = 12;

pub const MAX_METADATA_ENTRIES: u64 = 4096;
pub const MAX_STRING_BYTES: u64 = 1024 * 1024;
pub const MAX_ARRAY_ITEMS: u64 = 1024 * 1024;

/// Parsed metadata; `gguf.version`, `gguf.tensor_count` and
/// `gguf.metadata_count` are added from the header.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct GgufMetadata {
    pub fields: BTreeMap<String, Value>,
}

impl GgufMetadata {
    pub fn to_json(&self) -> Value {
        Value::Object(
            self.fields
                .iter()
                .map(|(k, v)| (k.clone(), v.clone()))
                .collect::<Map<String, Value>>(),
        )
    }

    pub fn int(&self, key: &str) -> Option<i64> {
        self.fields.get(key).and_then(Value::as_i64)
    }

    /// Non-empty trimmed string value.
    pub fn string(&self, key: &str) -> Option<String> {
        let value = self.fields.get(key)?.as_str()?.trim();
        (!value.is_empty()).then(|| value.to_string())
    }
}

struct Reader<R> {
    inner: R,
    len: u64,
}

impl<R: Read + Seek> Reader<R> {
    fn exact<const N: usize>(&mut self, what: &str) -> Result<[u8; N], String> {
        let mut buf = [0u8; N];
        self.inner
            .read_exact(&mut buf)
            .map_err(|_| format!("truncated GGUF {what}"))?;
        Ok(buf)
    }

    fn u32(&mut self, what: &str) -> Result<u32, String> {
        Ok(u32::from_le_bytes(self.exact(what)?))
    }

    fn u64(&mut self, what: &str) -> Result<u64, String> {
        Ok(u64::from_le_bytes(self.exact(what)?))
    }

    fn string(&mut self) -> Result<String, String> {
        let size = self
            .u64("string length")
            .map_err(|_| "truncated GGUF string length".to_string())?;
        if size > MAX_STRING_BYTES {
            return Err("GGUF string exceeds scanner limit".into());
        }
        let mut buf = vec![0u8; size as usize];
        self.inner
            .read_exact(&mut buf)
            .map_err(|_| "truncated GGUF string payload".to_string())?;
        Ok(String::from_utf8_lossy(&buf).into_owned())
    }

    fn skip(&mut self, bytes: u64) -> Result<(), String> {
        let offset = i64::try_from(bytes)
            .map_err(|_| "GGUF value exceeds stream offset limit".to_string())?;
        let position = self
            .inner
            .stream_position()
            .map_err(|_| "truncated GGUF value".to_string())?;
        let target = position
            .checked_add(offset as u64)
            .filter(|t| *t <= self.len);
        let Some(target) = target else {
            return Err("truncated GGUF value".into());
        };
        self.inner
            .seek(SeekFrom::Start(target))
            .map_err(|_| "truncated GGUF value".to_string())?;
        Ok(())
    }

    fn skip_value(&mut self, kind: u32) -> Result<(), String> {
        match kind {
            TYPE_STRING => self.string().map(|_| ()),
            TYPE_ARRAY => {
                let element = self
                    .u32("array header")
                    .map_err(|_| "truncated GGUF array header".to_string())?;
                let count = self
                    .u64("array header")
                    .map_err(|_| "truncated GGUF array header".to_string())?;
                if count > MAX_ARRAY_ITEMS {
                    return Err("GGUF array exceeds scanner limit".into());
                }
                match scalar_width(element) {
                    Some(width) => self.skip(count * width),
                    None => {
                        for _ in 0..count {
                            self.skip_value(element)?;
                        }
                        Ok(())
                    }
                }
            }
            other => match scalar_width(other) {
                Some(width) => self.skip(width),
                None => Err(format!("unsupported GGUF metadata type: {other}")),
            },
        }
    }

    fn value(&mut self, kind: u32) -> Result<Option<Value>, String> {
        let v = match kind {
            TYPE_UINT8 => Value::from(u8::from_le_bytes(self.exact("uint8")?)),
            TYPE_INT8 => Value::from(i8::from_le_bytes(self.exact("int8")?)),
            TYPE_UINT16 => Value::from(u16::from_le_bytes(self.exact("uint16")?)),
            TYPE_INT16 => Value::from(i16::from_le_bytes(self.exact("int16")?)),
            TYPE_UINT32 => Value::from(u32::from_le_bytes(self.exact("uint32")?)),
            TYPE_INT32 => Value::from(i32::from_le_bytes(self.exact("int32")?)),
            TYPE_BOOL => Value::from(u8::from_le_bytes(self.exact("bool")?) != 0),
            TYPE_STRING => Value::from(self.string()?),
            TYPE_UINT64 => Value::from(u64::from_le_bytes(self.exact("uint64")?)),
            TYPE_INT64 => Value::from(i64::from_le_bytes(self.exact("int64")?)),
            TYPE_FLOAT32 | TYPE_FLOAT64 | TYPE_ARRAY => {
                self.skip_value(kind)?;
                return Ok(None);
            }
            other => return Err(format!("unsupported GGUF metadata type: {other}")),
        };
        Ok(Some(v))
    }
}

fn scalar_width(kind: u32) -> Option<u64> {
    match kind {
        TYPE_UINT8 | TYPE_INT8 | TYPE_BOOL => Some(1),
        TYPE_UINT16 | TYPE_INT16 => Some(2),
        TYPE_UINT32 | TYPE_INT32 | TYPE_FLOAT32 => Some(4),
        TYPE_UINT64 | TYPE_INT64 | TYPE_FLOAT64 => Some(8),
        _ => None,
    }
}

/// Parse the metadata of the GGUF file at `path`. Errors use the archived
/// wording (`missing GGUF magic`, `truncated GGUF header`, ...).
pub fn read_metadata(path: &Path) -> Result<GgufMetadata, String> {
    let file = File::open(path).map_err(|_| "unable to open GGUF file".to_string())?;
    read_metadata_from(BufReader::new(file))
}

/// Parse GGUF metadata from any seekable reader.
pub fn read_metadata_from<R: Read + Seek>(mut input: R) -> Result<GgufMetadata, String> {
    let len = input
        .seek(SeekFrom::End(0))
        .and_then(|len| input.seek(SeekFrom::Start(0)).map(|_| len))
        .map_err(|_| "unable to open GGUF file".to_string())?;
    let mut reader = Reader { inner: input, len };
    let magic: [u8; 4] = reader
        .exact("magic")
        .map_err(|_| "missing GGUF magic".to_string())?;
    if &magic != b"GGUF" {
        return Err("missing GGUF magic".into());
    }
    let header = |r: &mut Reader<R>| -> Result<(u32, u64, u64), String> {
        Ok((r.u32("header")?, r.u64("header")?, r.u64("header")?))
    };
    let (version, tensor_count, metadata_count) =
        header(&mut reader).map_err(|_| "truncated GGUF header".to_string())?;
    if metadata_count > MAX_METADATA_ENTRIES {
        return Err("GGUF metadata entry count exceeds scanner limit".into());
    }
    let mut metadata = GgufMetadata::default();
    metadata
        .fields
        .insert("gguf.version".into(), Value::from(version));
    metadata
        .fields
        .insert("gguf.tensor_count".into(), Value::from(tensor_count));
    metadata
        .fields
        .insert("gguf.metadata_count".into(), Value::from(metadata_count));
    for _ in 0..metadata_count {
        let key = reader.string()?;
        let kind = reader
            .u32("metadata type")
            .map_err(|_| "truncated GGUF metadata type".to_string())?;
        if let Some(value) = reader.value(kind)? {
            metadata.fields.insert(key, value);
        }
    }
    Ok(metadata)
}

/// Builds GGUF files for tests.
#[cfg(test)]
pub(crate) mod testing {
    pub enum Kv<'a> {
        U32(u32),
        I32(i32),
        U64(u64),
        F32(f32),
        Bool(bool),
        Str(&'a str),
        StrArray(&'a [&'a str]),
        U32Array(&'a [u32]),
    }

    fn string(out: &mut Vec<u8>, s: &str) {
        out.extend((s.len() as u64).to_le_bytes());
        out.extend(s.as_bytes());
    }

    pub fn gguf(entries: &[(&str, Kv<'_>)]) -> Vec<u8> {
        let mut out = b"GGUF".to_vec();
        out.extend(3u32.to_le_bytes());
        out.extend(0u64.to_le_bytes());
        out.extend((entries.len() as u64).to_le_bytes());
        for (key, value) in entries {
            string(&mut out, key);
            match value {
                Kv::U32(v) => {
                    out.extend(4u32.to_le_bytes());
                    out.extend(v.to_le_bytes());
                }
                Kv::I32(v) => {
                    out.extend(5u32.to_le_bytes());
                    out.extend(v.to_le_bytes());
                }
                Kv::U64(v) => {
                    out.extend(10u32.to_le_bytes());
                    out.extend(v.to_le_bytes());
                }
                Kv::F32(v) => {
                    out.extend(6u32.to_le_bytes());
                    out.extend(v.to_le_bytes());
                }
                Kv::Bool(v) => {
                    out.extend(7u32.to_le_bytes());
                    out.push(u8::from(*v));
                }
                Kv::Str(s) => {
                    out.extend(8u32.to_le_bytes());
                    string(&mut out, s);
                }
                Kv::StrArray(items) => {
                    out.extend(9u32.to_le_bytes());
                    out.extend(8u32.to_le_bytes());
                    out.extend((items.len() as u64).to_le_bytes());
                    for item in *items {
                        string(&mut out, item);
                    }
                }
                Kv::U32Array(items) => {
                    out.extend(9u32.to_le_bytes());
                    out.extend(4u32.to_le_bytes());
                    out.extend((items.len() as u64).to_le_bytes());
                    for item in *items {
                        out.extend(item.to_le_bytes());
                    }
                }
            }
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::testing::{Kv, gguf};
    use super::*;
    use std::io::Cursor;

    #[test]
    fn reads_scalars_and_skips_floats_and_arrays() {
        let bytes = gguf(&[
            ("general.architecture", Kv::Str("qwen2")),
            ("qwen2.block_count", Kv::U32(24)),
            ("qwen2.rope.freq_base", Kv::F32(1e6)),
            ("tokenizer.ggml.tokens", Kv::StrArray(&["a", "b"])),
            ("tokenizer.ggml.token_type", Kv::U32Array(&[1, 2, 3])),
            ("general.file_type", Kv::U32(15)),
            ("x.signed", Kv::I32(-3)),
            ("x.big", Kv::U64(1 << 40)),
            ("x.flag", Kv::Bool(true)),
        ]);
        let meta = read_metadata_from(Cursor::new(bytes)).unwrap();
        assert_eq!(
            meta.string("general.architecture").as_deref(),
            Some("qwen2")
        );
        assert_eq!(meta.int("qwen2.block_count"), Some(24));
        assert_eq!(meta.int("general.file_type"), Some(15));
        assert_eq!(meta.int("x.signed"), Some(-3));
        assert_eq!(meta.int("x.big"), Some(1 << 40));
        assert_eq!(meta.fields["x.flag"], Value::Bool(true));
        assert!(!meta.fields.contains_key("qwen2.rope.freq_base"));
        assert!(!meta.fields.contains_key("tokenizer.ggml.tokens"));
        assert_eq!(meta.int("gguf.version"), Some(3));
        assert_eq!(meta.int("gguf.metadata_count"), Some(9));
        // Sorted keys in JSON.
        let keys: Vec<String> = meta
            .to_json()
            .as_object()
            .unwrap()
            .keys()
            .cloned()
            .collect();
        let mut sorted = keys.clone();
        sorted.sort();
        assert_eq!(keys, sorted);
    }

    #[test]
    fn reports_archived_errors() {
        assert_eq!(
            read_metadata_from(Cursor::new(b"GGML".to_vec())).unwrap_err(),
            "missing GGUF magic"
        );
        assert_eq!(
            read_metadata_from(Cursor::new(b"GGUF\x03\x00".to_vec())).unwrap_err(),
            "truncated GGUF header"
        );
        let mut huge = b"GGUF".to_vec();
        huge.extend(3u32.to_le_bytes());
        huge.extend(0u64.to_le_bytes());
        huge.extend(5000u64.to_le_bytes());
        assert_eq!(
            read_metadata_from(Cursor::new(huge)).unwrap_err(),
            "GGUF metadata entry count exceeds scanner limit"
        );
        let mut truncated = gguf(&[("general.name", Kv::Str("abcdef"))]);
        truncated.truncate(truncated.len() - 3);
        assert_eq!(
            read_metadata_from(Cursor::new(truncated)).unwrap_err(),
            "truncated GGUF string payload"
        );
        let mut bad_type = gguf(&[("k", Kv::U32(1))]);
        let len = bad_type.len();
        bad_type[len - 8..len - 4].copy_from_slice(&99u32.to_le_bytes());
        assert_eq!(
            read_metadata_from(Cursor::new(bad_type)).unwrap_err(),
            "unsupported GGUF metadata type: 99"
        );
        assert_eq!(
            read_metadata(Path::new("/nonexistent/x.gguf")).unwrap_err(),
            "unable to open GGUF file"
        );
    }
}
