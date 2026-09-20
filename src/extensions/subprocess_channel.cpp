#include <qt/extensions/subprocess_channel.hpp>

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace qt::extensions {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;
using qt::foundation::Status;

Status require_state(const SubprocessChannel& channel,
                     SubprocessChannelState expected, const char* operation) {
  if (channel.state == expected) {
    return Status::success();
  }

  return Status::failure(
      ErrorCode::invalid_argument,
      std::string(operation) + " requires channel state " +
          to_string(expected) + ", got " + to_string(channel.state));
}

Status require_io_state(const SubprocessChannel& channel, const char* operation) {
  if (channel.can_exchange_io()) {
    return Status::success();
  }

  return Status::failure(
      ErrorCode::invalid_argument,
      std::string(operation) +
          " requires channel state starting, running, or stopping, got " +
          to_string(channel.state));
}

std::string trim_line_ending(std::string line) {
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

template <typename Decoder>
Result<SubprocessChannelFrame> decode_stdout_line(std::string line,
                                                  SubprocessChannelFrameKind kind,
                                                  Decoder&& decoder) {
  const std::string normalized = trim_line_ending(line);
  const auto decoded = decoder(normalized + "\n");
  if (!decoded) {
    return Result<SubprocessChannelFrame>::failure(decoded.error().code,
                                                   decoded.error().message);
  }

  SubprocessChannelFrame frame;
  frame.stream = SubprocessChannelStream::kStdout;
  frame.kind = kind;
  frame.line = normalized;
  frame.payload = std::move(decoded.value());
  return frame;
}

Result<SubprocessChannelFrame> parse_stdout_line(std::string line) {
  const std::string normalized = trim_line_ending(line);
  const auto classified = classify_jsonrpc_line(normalized + "\n");
  if (!classified) {
    return Result<SubprocessChannelFrame>::failure(classified.error().code,
                                                   classified.error().message);
  }

  switch (classified.value()) {
    case JsonRpcMessageKind::kRequest:
      return decode_stdout_line(std::move(normalized),
                                SubprocessChannelFrameKind::kJsonRpcRequest,
                                decode_jsonrpc_request_line);
    case JsonRpcMessageKind::kNotification:
      return decode_stdout_line(std::move(normalized),
                                SubprocessChannelFrameKind::kJsonRpcNotification,
                                decode_jsonrpc_notification_line);
    case JsonRpcMessageKind::kResponse:
      return decode_stdout_line(std::move(normalized),
                                SubprocessChannelFrameKind::kJsonRpcResponse,
                                decode_jsonrpc_response_line);
    case JsonRpcMessageKind::kErrorResponse:
      return decode_stdout_line(std::move(normalized),
                                SubprocessChannelFrameKind::kJsonRpcErrorResponse,
                                decode_jsonrpc_error_response_line);
  }

  return Result<SubprocessChannelFrame>::failure(
      ErrorCode::internal_error, "unsupported stdout jsonrpc message kind");
}

Result<std::vector<SubprocessChannelFrame>> consume_stdout(
    SubprocessChannel& channel, std::string_view bytes) {
  channel.pending_stdout.append(bytes.data(), bytes.size());

  std::vector<SubprocessChannelFrame> frames;
  std::size_t offset = 0;
  while (true) {
    const std::size_t newline = channel.pending_stdout.find('\n', offset);
    if (newline == std::string::npos) {
      break;
    }

    auto frame = parse_stdout_line(
        channel.pending_stdout.substr(offset, newline - offset));
    if (!frame) {
      offset = newline + 1;
      channel.pending_stdout.erase(0, offset);
      return frame.error();
    }

    frames.push_back(std::move(frame.value()));
    offset = newline + 1;
  }

  if (offset > 0) {
    channel.pending_stdout.erase(0, offset);
  }
  return frames;
}

Result<std::vector<SubprocessChannelFrame>> consume_stderr(
    SubprocessChannel& channel, std::string_view bytes) {
  channel.pending_stderr.append(bytes.data(), bytes.size());

  std::vector<SubprocessChannelFrame> frames;
  std::size_t offset = 0;
  while (true) {
    const std::size_t newline = channel.pending_stderr.find('\n', offset);
    if (newline == std::string::npos) {
      break;
    }

    SubprocessChannelFrame frame;
    frame.stream = SubprocessChannelStream::kStderr;
    frame.kind = SubprocessChannelFrameKind::kDiagnostic;
    frame.line =
        trim_line_ending(channel.pending_stderr.substr(offset, newline - offset));
    frames.push_back(std::move(frame));
    offset = newline + 1;
  }

  if (offset > 0) {
    channel.pending_stderr.erase(0, offset);
  }
  return frames;
}

Result<std::string> encode_message(const SubprocessChannelMessage& message) {
  return std::visit(
      [](const auto& value) -> Result<std::string> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, JsonRpcRequest>) {
          return encode_jsonrpc_request_line(value);
        } else if constexpr (std::is_same_v<T, JsonRpcNotification>) {
          return encode_jsonrpc_notification_line(value);
        } else if constexpr (std::is_same_v<T, JsonRpcResponse>) {
          return encode_jsonrpc_response_line(value);
        } else {
          return encode_jsonrpc_error_response_line(value);
        }
      },
      message);
}

void reset_io_buffers(SubprocessChannel& channel) {
  channel.pending_stdin.clear();
  channel.pending_stdout.clear();
  channel.pending_stderr.clear();
}

}  // namespace

std::string to_string(SubprocessChannelState state) {
  switch (state) {
    case SubprocessChannelState::kStopped:
      return "stopped";
    case SubprocessChannelState::kStarting:
      return "starting";
    case SubprocessChannelState::kRunning:
      return "running";
    case SubprocessChannelState::kStopping:
      return "stopping";
  }

  return "stopped";
}

std::string to_string(SubprocessChannelStream stream) {
  switch (stream) {
    case SubprocessChannelStream::kStdout:
      return "stdout";
    case SubprocessChannelStream::kStderr:
      return "stderr";
  }

  return "stdout";
}

std::string to_string(SubprocessChannelFrameKind kind) {
  switch (kind) {
    case SubprocessChannelFrameKind::kJsonRpcRequest:
      return "jsonrpc_request";
    case SubprocessChannelFrameKind::kJsonRpcNotification:
      return "jsonrpc_notification";
    case SubprocessChannelFrameKind::kJsonRpcResponse:
      return "jsonrpc_response";
    case SubprocessChannelFrameKind::kJsonRpcErrorResponse:
      return "jsonrpc_error_response";
    case SubprocessChannelFrameKind::kDiagnostic:
      return "diagnostic";
  }

  return "diagnostic";
}

Result<SubprocessChannel> make_subprocess_channel() { return SubprocessChannel{}; }

Status request_subprocess_channel_start(SubprocessChannel& channel) {
  const Status state =
      require_state(channel, SubprocessChannelState::kStopped,
                    "request_subprocess_channel_start");
  if (!state) {
    return state;
  }

  reset_io_buffers(channel);
  channel.state = SubprocessChannelState::kStarting;
  return Status::success();
}

Status confirm_subprocess_channel_started(SubprocessChannel& channel) {
  const Status state =
      require_state(channel, SubprocessChannelState::kStarting,
                    "confirm_subprocess_channel_started");
  if (!state) {
    return state;
  }

  channel.state = SubprocessChannelState::kRunning;
  ++channel.open_count;
  return Status::success();
}

Status request_subprocess_channel_stop(SubprocessChannel& channel) {
  if (!channel.can_request_stop()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        "request_subprocess_channel_stop requires channel state starting or "
        "running, got " +
            to_string(channel.state));
  }

  channel.state = SubprocessChannelState::kStopping;
  return Status::success();
}

Status confirm_subprocess_channel_stopped(SubprocessChannel& channel) {
  const Status state =
      require_state(channel, SubprocessChannelState::kStopping,
                    "confirm_subprocess_channel_stopped");
  if (!state) {
    return state;
  }

  reset_io_buffers(channel);
  channel.state = SubprocessChannelState::kStopped;
  ++channel.close_count;
  return Status::success();
}

Status record_subprocess_channel_crashed(SubprocessChannel& channel) {
  if (channel.state == SubprocessChannelState::kStopped) {
    return Status::failure(
        ErrorCode::invalid_argument,
        "record_subprocess_channel_crashed requires channel state starting, "
        "running, or stopping, got stopped");
  }

  reset_io_buffers(channel);
  channel.state = SubprocessChannelState::kStopped;
  ++channel.close_count;
  return Status::success();
}

Status enqueue_subprocess_channel_message(SubprocessChannel& channel,
                                          const SubprocessChannelMessage& message) {
  const Status state = require_io_state(channel, "enqueue_subprocess_channel_message");
  if (!state) {
    return state;
  }

  const auto encoded = encode_message(message);
  if (!encoded) {
    return Status::failure(encoded.error().code, encoded.error().message);
  }

  channel.pending_stdin += encoded.value();
  return Status::success();
}

std::size_t pending_subprocess_channel_output_bytes(
    const SubprocessChannel& channel) noexcept {
  return channel.pending_stdin.size();
}

std::size_t pending_subprocess_channel_input_bytes(
    const SubprocessChannel& channel, SubprocessChannelStream stream) noexcept {
  switch (stream) {
    case SubprocessChannelStream::kStdout:
      return channel.pending_stdout.size();
    case SubprocessChannelStream::kStderr:
      return channel.pending_stderr.size();
  }

  return 0;
}

std::string take_subprocess_channel_output(SubprocessChannel& channel) {
  return take_subprocess_channel_output(
      channel, std::numeric_limits<std::size_t>::max());
}

std::string take_subprocess_channel_output(SubprocessChannel& channel,
                                           std::size_t max_bytes) {
  if (channel.pending_stdin.empty() || max_bytes == 0) {
    return "";
  }

  const std::size_t count = std::min(max_bytes, channel.pending_stdin.size());
  std::string output = channel.pending_stdin.substr(0, count);
  channel.pending_stdin.erase(0, count);
  return output;
}

Result<std::vector<SubprocessChannelFrame>> push_subprocess_channel_stdout(
    SubprocessChannel& channel, std::string_view bytes) {
  const Status state =
      require_io_state(channel, "push_subprocess_channel_stdout");
  if (!state) {
    return state.error();
  }
  return consume_stdout(channel, bytes);
}

Result<std::vector<SubprocessChannelFrame>> push_subprocess_channel_stderr(
    SubprocessChannel& channel, std::string_view bytes) {
  const Status state =
      require_io_state(channel, "push_subprocess_channel_stderr");
  if (!state) {
    return state.error();
  }
  return consume_stderr(channel, bytes);
}

}  // namespace qt::extensions
