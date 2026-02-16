{
  "targets": [
    {
      "target_name": "memcore",
      "sources": ["src/addon.cpp"],
      "cflags!": ["-fno-exceptions"],
      "cflags_cc!": ["-fno-exceptions"],
      "conditions": [
        ["OS=='linux'", {
          "libraries": ["-lpthread", "-lrt"],
          "defines": ["_GNU_SOURCE"]
        }],
        ["OS=='mac'", {
          "xcode_settings": {
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "CLANG_CXX_LIBRARY": "libc++"
          },
          "defines": ["_DARWIN_C_SOURCE"]
        }]
      ]
    }
  ]
}
