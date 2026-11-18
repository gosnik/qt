//! Line-delimited JSON-RPC 2.0 framing and validation (archived
//! `extensions/jsonrpc_transport.cpp` and `subprocess_channel.cpp`).

use serde_json::{Value, json};

pub const VERSION: &str = "2.0";
pub const METHOD_NOT_FOUND: i64 = -32601;

/// Method names of the extension protocol.
pub const COMMAND_INVOKE: &str = "qt.command.invoke";
pub const TOOL_INVOKE: &str = "qt.tool.invoke";
/// Lifecycle events are sent as `qt.event.<event>`.
pub const EVENT_PREFIX: &str = "qt.event.";

#[derive(Debug, Clone, PartialEq)]
pub enum Message {
    Request {
        id: Value,
        method: String,
        params: Value,
    },
    Notification {
        method: String,
        params: Value,
    },
    Response {
        id: Value,
        result: Value,
    },
    Error {
        id: Value,
        error: RpcError,
    },
}

#[derive(Debug, Clone, PartialEq)]
pub struct RpcError {
    pub code: i64,
    pub message: String,
    pub data: Value,
}

pub fn is_valid_request_id(id: &Value) -> bool {
    id.is_string() || id.is_i64() || id.is_u64()
}

pub fn is_valid_response_id(id: &Value) -> bool {
    id.is_null() || is_valid_request_id(id)
}

pub fn is_valid_params(params: &Value) -> bool {
    params.is_object() || params.is_array()
}

fn check_method(method: &str, context: &str) -> Result<(), String> {
    if method.is_empty() {
        return Err(format!("{context} method must not be empty"));
    }
    Ok(())
}

fn check_params(params: &Value, context: &str) -> Result<(), String> {
    if !is_valid_params(params) {
        return Err(format!("{context} params must be a JSON object or array"));
    }
    Ok(())
}

impl Message {
    pub fn request(id: Value, method: impl Into<String>, params: Value) -> Result<Self, String> {
        let method = method.into();
        check_method(&method, "jsonrpc request")?;
        check_params(&params, "jsonrpc request")?;
        if !is_valid_request_id(&id) {
            return Err("jsonrpc request id must be a string or integer".into());
        }
        Ok(Self::Request { id, method, params })
    }

    pub fn notification(method: impl Into<String>, params: Value) -> Result<Self, String> {
        let method = method.into();
        check_method(&method, "jsonrpc notification")?;
        check_params(&params, "jsonrpc notification")?;
        Ok(Self::Notification { method, params })
    }

    pub fn response(id: Value, result: Value) -> Result<Self, String> {
        if !is_valid_response_id(&id) {
            return Err("jsonrpc response id must be null, a string, or an integer".into());
        }
        Ok(Self::Response { id, result })
    }

    pub fn error(
        id: Value,
        code: i64,
        message: impl Into<String>,
        data: Value,
    ) -> Result<Self, String> {
        if !is_valid_response_id(&id) {
            return Err("jsonrpc error response id must be null, a string, or an integer".into());
        }
        let message = message.into();
        if message.is_empty() {
            return Err("jsonrpc error message must not be empty".into());
        }
        Ok(Self::Error {
            id,
            error: RpcError {
                code,
                message,
                data,
            },
        })
    }

    pub fn kind(&self) -> &'static str {
        match self {
            Self::Request { .. } => "request",
            Self::Notification { .. } => "notification",
            Self::Response { .. } => "response",
            Self::Error { .. } => "error_response",
        }
    }

    pub fn to_json(&self) -> Value {
        match self {
            Self::Request { id, method, params } => {
                json!({ "jsonrpc": VERSION, "method": method, "params": params, "id": id })
            }
            Self::Notification { method, params } => {
                json!({ "jsonrpc": VERSION, "method": method, "params": params })
            }
            Self::Response { id, result } => {
                json!({ "jsonrpc": VERSION, "id": id, "result": result })
            }
            Self::Error { id, error } => {
                let mut e = json!({ "code": error.code, "message": error.message });
                if !error.data.is_null() {
                    e["data"] = error.data.clone();
                }
                json!({ "jsonrpc": VERSION, "id": id, "error": e })
            }
        }
    }

    /// One frame: compact JSON plus `\n`.
    pub fn encode_line(&self) -> String {
        let mut line = self.to_json().to_string();
        line.push('\n');
        line
    }

    /// Classify and validate a decoded JSON-RPC message.
    pub fn from_json(json: &Value) -> Result<Self, String> {
        let obj = json
            .as_object()
            .ok_or_else(|| "jsonrpc message must be a JSON object".to_string())?;
        if let Some(v) = obj.get("jsonrpc")
            && v.as_str() != Some(VERSION)
        {
            return Err("jsonrpc field must equal '2.0'".into());
        }
        let has_result = obj.contains_key("result");
        let has_error = obj.contains_key("error");
        if let Some(method) = obj.get("method") {
            if has_result || has_error {
                return Err("jsonrpc method message must not contain result or error".into());
            }
            let method = method
                .as_str()
                .ok_or_else(|| "jsonrpc method must be a string".to_string())?;
            let params = obj.get("params").cloned().unwrap_or_else(|| json!({}));
            return match obj.get("id") {
                Some(id) => Self::request(id.clone(), method, params),
                None => Self::notification(method, params),
            };
        }
        let id = obj
            .get("id")
            .ok_or_else(|| "jsonrpc response must contain id".to_string())?;
        if has_result == has_error {
            return Err("jsonrpc response must contain exactly one of result or error".into());
        }
        if has_result {
            return Self::response(id.clone(), obj["result"].clone());
        }
        let error = obj["error"]
            .as_object()
            .ok_or_else(|| "jsonrpc error must be a JSON object".to_string())?;
        let code = error
            .get("code")
            .and_then(Value::as_i64)
            .ok_or_else(|| "jsonrpc error code must be an integer".to_string())?;
        let message = error
            .get("message")
            .and_then(Value::as_str)
            .ok_or_else(|| "jsonrpc error message must be a string".to_string())?;
        Self::error(
            id.clone(),
            code,
            message,
            error.get("data").cloned().unwrap_or(Value::Null),
        )
    }

    /// Decode exactly one line (an optional trailing `\n` / `\r\n` is allowed).
    pub fn decode_line(line: &str) -> Result<Self, String> {
        if line.is_empty() {
            return Err("jsonrpc frame must not be empty".into());
        }
        let payload = line
            .strip_suffix('\n')
            .map(|l| l.strip_suffix('\r').unwrap_or(l))
            .unwrap_or(line);
        if payload.is_empty() {
            return Err("jsonrpc frame payload must not be empty".into());
        }
        if payload.contains(['\n', '\r']) {
            return Err("jsonrpc frame must contain exactly one line".into());
        }
        let value: Value = serde_json::from_str(payload).map_err(|e| e.to_string())?;
        Self::from_json(&value)
    }
}

/// Splits a byte stream into complete lines.
#[derive(Debug, Default)]
pub struct LineBuffer {
    pending: Vec<u8>,
}

impl LineBuffer {
    /// Append bytes and return the complete lines (without line endings).
    pub fn push(&mut self, bytes: &[u8]) -> Vec<String> {
        self.pending.extend_from_slice(bytes);
        let mut lines = Vec::new();
        while let Some(pos) = self.pending.iter().position(|b| *b == b'\n') {
            let mut line: Vec<u8> = self.pending.drain(..=pos).collect();
            line.pop();
            if line.last() == Some(&b'\r') {
                line.pop();
            }
            lines.push(String::from_utf8_lossy(&line).into_owned());
        }
        lines
    }

    /// Whatever is left after the stream ended (an unterminated last line).
    pub fn finish(&mut self) -> Option<String> {
        let rest = std::mem::take(&mut self.pending);
        (!rest.is_empty()).then(|| String::from_utf8_lossy(&rest).into_owned())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips_and_classifies() {
        let r = Message::request(json!(1), "qt.tool.invoke", json!({"name":"x"})).unwrap();
        assert_eq!(
            r.encode_line(),
            "{\"jsonrpc\":\"2.0\",\"method\":\"qt.tool.invoke\",\"params\":{\"name\":\"x\"},\"id\":1}\n"
        );
        assert_eq!(Message::decode_line(&r.encode_line()).unwrap(), r);
        let n = Message::decode_line(r#"{"jsonrpc":"2.0","method":"log"}"#).unwrap();
        assert_eq!(n.kind(), "notification");
        let e = Message::decode_line(
            r#"{"jsonrpc":"2.0","id":"a","error":{"code":-1,"message":"bad","data":{"x":1}}}"#,
        )
        .unwrap();
        assert_eq!(e.to_json()["error"]["data"], json!({"x":1}));
    }

    #[test]
    fn validation_messages() {
        let err = |l: &str| Message::decode_line(l).unwrap_err();
        assert_eq!(err(""), "jsonrpc frame must not be empty");
        assert_eq!(err("\n"), "jsonrpc frame payload must not be empty");
        assert_eq!(err("{}\n{}"), "jsonrpc frame must contain exactly one line");
        assert_eq!(err("[]"), "jsonrpc message must be a JSON object");
        assert_eq!(
            err(r#"{"jsonrpc":"1.0","id":1,"result":1}"#),
            "jsonrpc field must equal '2.0'"
        );
        assert_eq!(err(r#"{"result":1}"#), "jsonrpc response must contain id");
        assert_eq!(
            err(r#"{"id":1,"result":1,"error":{}}"#),
            "jsonrpc response must contain exactly one of result or error"
        );
        assert_eq!(
            err(r#"{"id":1,"method":"m","result":1}"#),
            "jsonrpc method message must not contain result or error"
        );
        assert_eq!(
            err(r#"{"id":1.5,"method":"m"}"#),
            "jsonrpc request id must be a string or integer"
        );
        assert_eq!(
            err(r#"{"id":1,"method":"m","params":3}"#),
            "jsonrpc request params must be a JSON object or array"
        );
        assert_eq!(
            err(r#"{"id":1,"method":""}"#),
            "jsonrpc request method must not be empty"
        );
        assert_eq!(
            err(r#"{"id":1,"error":{"code":1,"message":""}}"#),
            "jsonrpc error message must not be empty"
        );
    }

    #[test]
    fn line_buffer() {
        let mut b = LineBuffer::default();
        assert!(b.push(b"ab").is_empty());
        assert_eq!(
            b.push(b"c\r\nd\ne"),
            vec!["abc".to_string(), "d".to_string()]
        );
        assert_eq!(b.finish().as_deref(), Some("e"));
    }
}
