#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <qt/extensions/jsonrpc_transport.hpp>
#include <qt/foundation/error.hpp>

namespace qt::extensions {

enum class SubprocessChannelState {
  kStopped,
  kStarting,
  kRunning,
  kStopping,
};

enum class SubprocessChannelStream {
  kStdout,
  kStderr,
};

enum class SubprocessChannelFrameKind {
  kJsonRpcRequest,
  kJsonRpcNotification,
  kJsonRpcResponse,
  kJsonRpcErrorResponse,
  kDiagnostic,
};

using SubprocessChannelMessage =
    std::variant<JsonRpcRequest, JsonRpcNotification, JsonRpcResponse,
                 JsonRpcErrorResponse>;

using SubprocessChannelPayload =
    std::variant<std::monostate, JsonRpcRequest, JsonRpcNotification,
                 JsonRpcResponse, JsonRpcErrorResponse>;

struct SubprocessChannelFrame {
  SubprocessChannelStream stream = SubprocessChannelStream::kStdout;
  SubprocessChannelFrameKind kind = SubprocessChannelFrameKind::kDiagnostic;
  std::string line;
  SubprocessChannelPayload payload = std::monostate{};

  bool is_jsonrpc() const noexcept {
    return kind != SubprocessChannelFrameKind::kDiagnostic;
  }
};

struct SubprocessChannel {
  SubprocessChannelState state = SubprocessChannelState::kStopped;
  std::string pending_stdin;
  std::string pending_stdout;
  std::string pending_stderr;
  std::size_t open_count = 0;
  std::size_t close_count = 0;

  bool can_request_start() const noexcept {
    return state == SubprocessChannelState::kStopped;
  }

  bool can_request_stop() const noexcept {
    return state == SubprocessChannelState::kStarting ||
           state == SubprocessChannelState::kRunning;
  }

  bool can_exchange_io() const noexcept {
    return state == SubprocessChannelState::kStarting ||
           state == SubprocessChannelState::kRunning ||
           state == SubprocessChannelState::kStopping;
  }
};

std::string to_string(SubprocessChannelState state);
std::string to_string(SubprocessChannelStream stream);
std::string to_string(SubprocessChannelFrameKind kind);

qt::foundation::Result<SubprocessChannel> make_subprocess_channel();

qt::foundation::Status request_subprocess_channel_start(
    SubprocessChannel& channel);
qt::foundation::Status confirm_subprocess_channel_started(
    SubprocessChannel& channel);
qt::foundation::Status request_subprocess_channel_stop(
    SubprocessChannel& channel);
qt::foundation::Status confirm_subprocess_channel_stopped(
    SubprocessChannel& channel);
qt::foundation::Status record_subprocess_channel_crashed(
    SubprocessChannel& channel);

qt::foundation::Status enqueue_subprocess_channel_message(
    SubprocessChannel& channel, const SubprocessChannelMessage& message);

std::size_t pending_subprocess_channel_output_bytes(
    const SubprocessChannel& channel) noexcept;
std::size_t pending_subprocess_channel_input_bytes(
    const SubprocessChannel& channel, SubprocessChannelStream stream) noexcept;

std::string take_subprocess_channel_output(SubprocessChannel& channel);
std::string take_subprocess_channel_output(SubprocessChannel& channel,
                                           std::size_t max_bytes);

qt::foundation::Result<std::vector<SubprocessChannelFrame>>
push_subprocess_channel_stdout(SubprocessChannel& channel, std::string_view bytes);

qt::foundation::Result<std::vector<SubprocessChannelFrame>>
push_subprocess_channel_stderr(SubprocessChannel& channel, std::string_view bytes);

}  // namespace qt::extensions
