//
// Arquiteturas de Alto Desempenho 2026/2027 --- assignment A1
//
// WebAssembly (emscripten) entry point
//
// the emscripten 3.1.6 package of Ubuntu 24.04 (which uses clang 15) silently drops a main(argc,argv)
// function: the .wasm file ends up almost empty (about 60 bytes) and running it does nothing; a
// main(void) function works with every emscripten version, so here main(void) fetches the command
// line arguments from the JavaScript side (node: process.argv, browser: Module.arguments) and calls
// the real main function
//
// usage (just before the definition of main):
//   #ifdef __EMSCRIPTEN__
//   # include "sha1_wasm_main.h"
//   #endif
//

#ifndef SHA1_WASM_MAIN
#define SHA1_WASM_MAIN

#include <emscripten.h>

// number of arguments (including the program name)
EM_JS(int,aad_wasm_argc,(void),{
  return arguments_.length + 1;
});

// copy argument i (0 is the program name) to buf (NUL terminated, truncated to size bytes)
EM_JS(void,aad_wasm_argv,(int i,char *buf,int size),{
  var b = new TextEncoder().encode((i == 0) ? thisProgram : String(arguments_[i - 1]));
  var n = Math.min(b.length,size - 1);
  HEAPU8.set(b.subarray(0,n),buf);
  HEAPU8[buf + n] = 0;
});

int aad_wasm_real_main(int argc,char **argv);

int main(void)
{
  int argc = aad_wasm_argc();
  char **argv = (char **)calloc((size_t)argc + 1u,sizeof(char *));

  for(int i = 0;i < argc;i++)
  {
    argv[i] = (char *)malloc(1024u);
    aad_wasm_argv(i,argv[i],1024);
  }
  return aad_wasm_real_main(argc,argv);
}

#define main aad_wasm_real_main

#endif
