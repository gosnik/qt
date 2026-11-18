#include "qt/app/interactive/interactive_turn_pump.hpp"

#include <iostream>
#include <stdexcept>

int main() {
  try {
    using namespace qt::app;
    const auto require = [](bool ok, const char* message) {
      if (!ok) throw std::runtime_error(message);
    };
    InteractiveTurnPump pump([](InteractiveTurnPumpRequest) {
      return qt::foundation::Result<std::string>::success("done");
    });
    require(!pump.queue_operation({InteractiveTurnPumpOperationKind::kSteer, "inactive"}),
            "inactive queue accepted work");
    require(pump.submit({}).value(), "turn was not accepted");
    for (std::size_t index = 0; index < 128U; ++index) {
      require(pump.queue_operation({InteractiveTurnPumpOperationKind::kSteer,
                                   std::to_string(index)}), "queue rejected admitted work");
    }
    require(!pump.queue_operation({InteractiveTurnPumpOperationKind::kSteer, "overflow"}),
            "queue exceeded its count limit");
    const auto drained = pump.drain_queued_operations();
    require(drained.size() == 128U, "queue lost accepted operations");
    for (std::size_t index = 0; index < drained.size(); ++index) {
      require(drained[index].text == std::to_string(index), "queue changed FIFO order");
    }
    require(pump.queue_operation({InteractiveTurnPumpOperationKind::kFollowUp,
                                 std::string(8U * 1024U * 1024U, 'x')}),
            "queue failed to reset its byte budget after draining");
    require(!pump.queue_operation({InteractiveTurnPumpOperationKind::kSteer, "x"}),
            "queue exceeded its byte budget");
    require(pump.drain_queued_operations().size() == 1U, "byte limit lost accepted work");
    require(static_cast<bool>(pump.poll(true)), "turn failed to finish");
    std::cout << "turn queue probe: ok\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
