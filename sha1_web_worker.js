//
// Arquiteturas de Alto Desempenho 2026/2027 --- assignment A1
//
// Web Worker that runs sha1_find (compiled to WebAssembly by emscripten) inside the browser,
// so that the main page stays responsive; several workers can run at the same time (one per core)
//
// message from the page: { build: "sha1_web_simd.js" or "sha1_web_scalar.js",
//                          args: [ command line arguments of sha1_find ],
//                          input: { name: "file.pdf", bytes: ArrayBuffer },
//                          output: name of the output file or null }
// messages to the page:  { type: "line", text: "..." }       --- a line printed by sha1_find
//                        { type: "done", status: n, csv: "..." or null, output: ArrayBuffer or null }
//                        { type: "error", text: "..." }
//

self.onmessage = function(ev)
{
  const job = ev.data;

  try
  {
    importScripts(job.build); // defines createSha1Find()
  }
  catch(e)
  {
    self.postMessage({ type: "error", text: "unable to load " + job.build + ": " + e });
    return;
  }
  createSha1Find({
    arguments: job.args,                       // read by main() (see sha1_wasm_main.h)
    print: (text) => self.postMessage({ type: "line", text: text }),
    printErr: (text) => self.postMessage({ type: "line", text: text })
  }).then(function(Module)
  {
    let status = 0;

    Module.FS.writeFile(job.input.name,new Uint8Array(job.input.bytes));
    try
    {
      status = Module.callMain(job.args);      // runs sha1_find to the end (synchronously)
    }
    catch(e)
    { // exit() may throw an ExitStatus object
      status = (e && typeof e.status === "number") ? e.status : 1;
      if(!(e && e.name === "ExitStatus"))
        self.postMessage({ type: "line", text: "exception: " + e });
    }
    let csv = null,output = null;
    try { csv = Module.FS.readFile("/results.csv",{ encoding: "utf8" }); } catch(e) { }
    if(job.output)
      try { output = Module.FS.readFile(job.output).buffer; } catch(e) { }
    self.postMessage({ type: "done", status: (status === undefined) ? 0 : status, csv: csv, output: output },output ? [ output ] : []);
  },function(e)
  {
    self.postMessage({ type: "error", text: "unable to start the WebAssembly module: " + e });
  });
};
