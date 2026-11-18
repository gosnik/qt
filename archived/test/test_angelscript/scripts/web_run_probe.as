void register_tests() {
  qt_test("Optimized web access batches search and defers bounded navigation",
          "test_web_run_probe");
}

void test_web_run_probe() {
  qt_assert(qt_run_web_run_probe(), "Optimized web access probe should pass");
}
