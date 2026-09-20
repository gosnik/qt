void register_tests() {
  qt_test("Global prompt history is bounded, private, recoverable, and " +
          "concurrency-safe",
          "test_prompt_history_probe");
}

void test_prompt_history_probe() {
  qt_assert(qt_run_prompt_history_probe(),
            "Global prompt history probe should pass");
}
