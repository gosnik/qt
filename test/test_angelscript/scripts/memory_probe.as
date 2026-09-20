void register_tests() {
  qt_test("Built-in memory persists scoped governed context without MCP",
          "test_memory_probe");
  qt_test("Governed memory review contracts pass through the AngelScript host",
          "test_memory_review_probe");
}

void test_memory_probe() {
  qt_assert(qt_run_memory_probe(),
            "Built-in native memory probe should pass");
}

void test_memory_review_probe() {
  qt_assert(qt_run_memory_review_probe(),
            "Governed memory review probe should pass");
}
