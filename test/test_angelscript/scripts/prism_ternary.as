void register_tests() {
  qt_test("Built-in llama exposes the PrismML ternary GGUF ABI",
          "test_prism_ternary_format");
}

void test_prism_ternary_format() {
  qt_assert(qt_prism_ternary_format_supported(),
            "Prism Q2_0 must use ftype 41, tensor type 42, and 128-value/34-byte blocks");
}
